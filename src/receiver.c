#include "receiver.h"

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "crc.h"
#include "deinterleave.h"
#include "demod.h"
#include "dewhiten.h"
#include "frame_sync.h"
#include "hamming.h"
#include "header.h"
#include "logging.h"
#include "output.h"
#include "source.h"

/*
 * The chain, one function per stage:
 *
 *   detect_frame -> sync_frame -> decode_frame (header handshake inside)
 *
 * next_symbol is the only place that turns source samples into LLRs,
 * decode_block the only place that turns a block's LLRs into nibbles. Stages
 * own no I/O; report_frame renders the human verdict on stderr and the
 * structured record on stdout. Header and payload both decode soft
 * (demod_soft_llrs -> deinterleave_soft -> hamming_decode_soft).
 *
 * Reference: [gr-lora_sdr] lora_RX.grc:735-741; [gr-lora] decode_impl.cc:421.
 */

#define MAX_SF 12        // widest LLR row: one chip per bit position
#define HEADER_SYMBOLS 8 // the header block is always 8 symbols

/* Rewind depth past a bad frame's fire, in whole symbol windows. Deeper skips
 * re-decode less but can step over a real preamble hiding within SKIP windows
 * of the fire. */
#define REWIND_SKIP_SYMS 4
#define MAX_BYTES 255 // the PHY length field is one byte

struct receiver
{
    struct frame_sync *fs;
    struct demod_state demod; // allocated once, reset per frame
    struct chase_config chase;
    int report_crc_bad; // emit CRC-invalid frames on stdout (--report-bad)
};

// outcome of one frame, one value per termination path
enum frame_decode
{
    DECODE_CRC_OK,   // payload complete, CRC valid
    DECODE_NO_CRC,   // payload complete, no CRC in the frame
    DECODE_CRC_BAD,  // payload complete, CRC invalid
    DECODE_HEADER,   // header block failed (short window or checksum)
    DECODE_TRUNCATED // payload ended before its budget
};

// what a completed frame yielded; the caller renders it
struct frame_result
{
    uint8_t bytes[MAX_BYTES];
    uint32_t len;
    int has_crc;
    float snr_db; // header symbols' mean on-channel SNR
};

/*
 * The chain names absolute positions in its working grid (bw*os); a capture
 * reader works in the file's own samples. Emit that mapping on a trace line
 * beside each absolute-position report.
 */
static void log_input_sample(struct source *src, size_t pos)
{
    (void)src;
    log_debug("grid sample %zu\n", pos);
}

/*
 * Produce one payload window and demodulate it: the single window -> LLR
 * seam. On success llr[] holds the window's sf bit reliabilities and *pos
 * has advanced by the SFO-corrected consume count.
 */
static int next_symbol(
    struct source *src,
    struct frame_sync *fs,
    struct demod_state *fd,
    size_t *pos,
    float *llr,
    struct frame_sync_status *st)
{
    const float complex *w = source_window(src, *pos, *pos + st->samples_per_symbol);
    uint32_t advance;
    const float complex *out = w ? frame_sync_output_step(fs, w, &advance) : NULL;

    if (!out)
        return -1;

    frame_sync_status(fs, st);

    (void)demod_step(fd, out); // hard value unused: soft only
    demod_soft_llrs(fd, llr);
    *pos += advance;

    return 0;
}

// one payload codeword's runner-up candidate, its flip LLR loss (best minus
// second-best soft score) and the codeword's LLR mass
struct cw_suspect
{
    uint8_t alt; // runner-up candidate nibble
    float marg;  // best minus second-best soft score
    float norm;  // the codeword's LLR mass
};

/*
 * Deinterleave one block and soft-decode its codewords: the single
 * block-LLRs -> nibbles seam. Header: 8 symbols, sf-2 codewords, cr 4.
 * Payload: cr+4 symbols, sf codewords, the frame's cr.
 *
 * @param suspects optionally receives each codeword's runner-up record;
 *        NULL skips
 *
 * @return the number of nibbles written
 */
static int decode_block(
    const float (*sym_llr)[MAX_SF],
    int n_sym,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors deinterleave_soft's signature
    int n_cw,
    int cr,
    uint8_t *nib,
    struct cw_suspect *suspects)
{
    float cw_llr[MAX_SF][MAX_SF];
    int i;

    // n_sym/n_cw/cr arrive as literals or header-validated values (header.c
    // rejects cr outside 1..4); deinterleave_soft's stack blocks are sized
    // for the maxima above
    deinterleave_soft(sym_llr, (uint8_t)n_sym, (uint8_t)n_cw, cw_llr);

    for (i = 0; i < n_cw; i++)
    {
        if (suspects)
        {
            float s = 0;
            for (uint8_t j = 0; j < (uint8_t)(cr + 4); j++)
                s += fabsf(cw_llr[i][j]);
            suspects[i].norm = s;
        }
        nib[i] = hamming_decode_soft(
            cw_llr[i],
            (uint8_t)cr,
            suspects ? &suspects[i].alt : NULL,
            suspects ? &suspects[i].marg : NULL);
    }

    return n_cw;
}

/*
 * Assemble a completed frame: dewhiten the payload nibbles, then the CRC
 * verdict. Pure data in, verdict + bytes out — no logging.
 */
static enum frame_decode
finish_frame(const uint8_t *nibs, const struct lora_header *h, struct frame_result *out)
{
    uint32_t p;
    uint16_t crc_rx, crc_calc;

    // both callers only assemble once nnib reached 2*pay_len + the crc nibbles
    dewhiten(nibs, h->pay_len, out->bytes); // payload nibbles only: CRC nibbles stay raw

    out->len = h->pay_len;
    out->has_crc = h->has_crc;

    if (!h->has_crc)
        return DECODE_NO_CRC;

    if (h->pay_len < 2)
        return DECODE_CRC_BAD; // CRC undefined for empty payloads

    p = 2u * h->pay_len;
    crc_rx = (uint16_t)(nibs[p] | (nibs[p + 1] << 4) | (nibs[p + 2] << 8) |
                        ((uint16_t)nibs[p + 3] << 12));
    crc_calc = crc16_compute(out->bytes, h->pay_len - 2);
    crc_calc =
        (uint16_t)(crc_calc ^ out->bytes[h->pay_len - 1] ^ (out->bytes[h->pay_len - 2] << 8));

    return crc_rx == crc_calc ? DECODE_CRC_OK : DECODE_CRC_BAD;
}

/*
 * Chase retry, CRC-arbitrated: from the K least-reliable payload codewords
 * (suspects: margin under CHASE_MARG of their LLR mass), enumerate runner-up
 * flips, singles then pairs; a CRC-passing combo is accepted by lowest total
 * flip LLR loss, within the budget cc->tau — a wrong combo must be both
 * CRC-consistent and metrically plausible.
 *
 * @return 1 on a rescued frame (out filled), 0 if no combo verified
 */
#define CHASE_MARG 0.2f // a suspect's margin must be under this share of its LLR mass
#define CHASE_K 8       // least-reliable codewords considered
#define CHASE_TAU 1.0f  // max total flip LLR loss to accept

static int chase_retry(
    const uint8_t *nibs,
    const struct cw_suspect *suspects,
    uint32_t nnib,
    uint32_t pay_nib,
    const struct lora_header *h,
    struct frame_result *out)
{
    uint8_t tmp[2 * MAX_BYTES + 4], used[2 * MAX_BYTES + 4] = {0};
    uint32_t sel[2 * MAX_BYTES + 4];
    struct frame_result res, best_res;
    float best_loss = 0;
    int k = CHASE_K, n_sel = 0, s, t, found = 0;

    if (pay_nib == 0 || pay_nib > nnib)
        return 0;
    if (pay_nib < (uint32_t)k)
        k = (int)pay_nib;

    // suspects: the k payload nibbles with the smallest margins; the CRC
    // nibbles are appended outside the K budget — they must not crowd
    // payload suspects out
    for (s = 0; s < k; s++)
    {
        uint32_t best = pay_nib;
        for (uint32_t i = 0; i < pay_nib; i++)
        {
            if (used[i])
                continue;
            if (suspects[i].marg > CHASE_MARG * suspects[i].norm)
                continue;
            if (best == pay_nib || suspects[i].marg < suspects[best].marg)
                best = i;
        }
        if (best == pay_nib)
            break;
        used[best] = 1;
        sel[n_sel++] = best;
    }
    for (uint32_t i = pay_nib; i < nnib; i++) // the CRC nibbles, same gate
        if (suspects[i].marg <= CHASE_MARG * suspects[i].norm)
            sel[n_sel++] = i;

    // singles, arbitrated by loss like the pairs: sel's CRC tail is unsorted
    for (s = 0; s < n_sel; s++)
    {
        float loss = suspects[sel[s]].marg;
        memcpy(tmp, nibs, nnib);
        tmp[sel[s]] = suspects[sel[s]].alt;
        if (finish_frame(tmp, h, &res) == DECODE_CRC_OK && (!found || loss < best_loss))
        {
            found = 1;
            best_loss = loss;
            best_res = res;
        }
    }

    // pairs
    for (s = 1; s < n_sel; s++)
        for (t = 0; t < s; t++)
        {
            float loss = suspects[sel[s]].marg + suspects[sel[t]].marg;
            memcpy(tmp, nibs, nnib);
            tmp[sel[s]] = suspects[sel[s]].alt;
            tmp[sel[t]] = suspects[sel[t]].alt;
            if (finish_frame(tmp, h, &res) == DECODE_CRC_OK && (!found || loss < best_loss))
            {
                found = 1;
                best_loss = loss;
                best_res = res;
            }
        }

    if (!found || best_loss > CHASE_TAU)
        return 0;
    *out = best_res;
    log_info("chase: rescued, flip LLR loss %.1f\n", (double)best_loss);
    return 1;
}

/*
 * Decode one frame: header block -> handshake -> payload blocks.
 *
 * The header block unlocks the symbol budget (frame_sync_set_symbol_budget);
 * everything downstream — how many windows next_symbol may still produce —
 * keys off it.
 */
static enum frame_decode decode_frame(
    struct source *src,
    struct frame_sync *fs,
    struct demod_state *fd,
    size_t *pos,
    struct frame_result *out,
    struct frame_sync_status *st,
    const struct chase_config *cc)
{
    float sllr[HEADER_SYMBOLS][MAX_SF];
    uint8_t hnib[MAX_SF], nibs[2 * MAX_BYTES + 4];
    struct cw_suspect suspects[2 * MAX_BYTES + 4]; // chase-retry suspects per codeword
    uint32_t nnib = 0, needed, i;
    uint8_t cw_len, nb = 0;
    struct lora_header h;
    const uint8_t sf = (uint8_t)st->sf;
    float snr_acc = 0; // header symbols' linear on-channel SNR estimates

    if (sf < 5 || sf > MAX_SF) // bounds the fixed-size stack blocks below
        return DECODE_HEADER;

    // header block: 8 symbols -> sf-2 codewords -> nibbles, soft
    for (i = 0; i < HEADER_SYMBOLS; i++)
    {
        if (next_symbol(src, fs, fd, pos, sllr[i], st) < 0)
        {
            log_warn("frame too short for the header\n");
            return DECODE_HEADER;
        }
        snr_acc += demod_snr(fd);
    }

    // per-frame SNR report: the header symbols' mean on-channel SNR, in dB,
    // referenced to the full channel bandwidth
    out->snr_db = 10.f * log10f(snr_acc / (float)HEADER_SYMBOLS);

    if (decode_block(sllr, HEADER_SYMBOLS, sf - 2, 4, hnib, NULL) < 0)
        return DECODE_HEADER;

    if (decode_header(hnib, &h) != 0)
    {
        log_warn("header checksum invalid or empty frame\n");
        return DECODE_HEADER;
    }

    // explicit unsigned: uint8_t promotes to int, which is UB for %u
    log_info(
        "header: pay_len=%u cr=4/%u crc=%d snr=%+.1f dB\n",
        (unsigned)h.pay_len,
        (unsigned)((uint8_t)(h.cr + 4)),
        h.has_crc,
        (double)out->snr_db);

    // the header block yields sf-2 nibbles: 5 header + passthrough payload
    // ([gr-lora_sdr] header_decoder_impl.cc:174-178)
    for (i = 5; i < (uint32_t)(sf - 2); i++)
        nibs[nnib++] = hnib[i];

    // caller-side handshake: unlock the payload budget
    frame_sync_set_symbol_budget(fs, decode_symbol_budget(h.pay_len, h.cr, h.has_crc, sf));

    // payload blocks of cr+4 symbols -> sf nibbles each, soft-decoded.
    // Without --chase nothing consumes the suspects: the soft scan runs
    // plain-argmax and the copies stop
    const int chase_on = cc->enabled;
    cw_len = (uint8_t)(h.cr + 4);
    needed = 2u * h.pay_len + (h.has_crc ? 4u : 0u);
    while (nnib < needed)
    {
        uint8_t pnib[MAX_SF];
        struct cw_suspect psus[MAX_SF];
        int n;

        if (next_symbol(src, fs, fd, pos, sllr[nb], st) < 0)
            break; // stream spent or budget spent
        if (++nb < cw_len)
            continue;

        n = decode_block(sllr, cw_len, sf, h.cr, pnib, chase_on ? psus : NULL);
        for (i = 0; i < (uint32_t)n && nnib < needed; i++)
        {
            nibs[nnib] = pnib[i];
            if (chase_on)
                suspects[nnib] = psus[i];
            nnib++;
        }
        nb = 0;
    }
    if (nnib < needed)
    {
        log_warn("frame ended after %u of %u payload nibbles\n", nnib, needed);
        return DECODE_TRUNCATED;
    }

    enum frame_decode verdict = finish_frame(nibs, &h, out);
    if (verdict == DECODE_CRC_BAD && cc->enabled &&
        chase_retry(nibs, suspects, nnib, 2u * h.pay_len, &h, out))
        verdict = DECODE_CRC_OK;
    return verdict;
}

/*
 * Hunt the preamble one window at a time; on a fire, advance to the first
 * network-id symbol and return 1. Returns 0 at end of stream, -1 on error.
 * The look-ahead source_advance() keeps the sync word's history readable.
 * fire_pos receives the fire window's own position, before the coarse jump:
 * the caller rewinds to just past it when the frame verdict comes back bad.
 */
static int detect_frame(
    struct source *src,
    struct frame_sync *fs,
    size_t *pos,
    struct frame_sync_status *st,
    size_t *fire_pos)
{
    const float complex *w;

    while ((w = source_window(src, *pos, *pos + st->samples_per_symbol)))
    {
        int64_t advance = 0;
        int ret = frame_sync_detect_step(fs, w, &advance);
        frame_sync_status(fs, st);

        if (ret > 0)
        {
            /* advance = os*(N - k_hat) + (lead-1)*sps; negative when the
             * sync word starts inside the fire window, clamped at 0 as in
             * the reference */
            int64_t next = (int64_t)*pos + advance;

            *fire_pos = *pos; // the fire window itself, ahead of the coarse jump
            *pos = next > 0 ? (size_t)next : 0;
            log_debug(
                "frame detected: k_hat=%d over %d windows -> coarse sync: "
                "discard %lld samples (next symbol window at sample %zu)\n",
                st->k_hat,
                st->n_up_req,
                (long long)advance,
                *pos);
            log_input_sample(src, *pos);
            return 1;
        }

        *pos += st->samples_per_symbol; // DETECT consumes one window per step
        /* keep the declared look-ahead as history: the sync word can lie
         * up to SYNC_LOOKAHEAD windows behind the fire position */
        source_advance(src, *pos);
    }
    return 0;
}

/*
 * Walk SYNC's five sub-states; returns 1 when the sync word is accepted (and
 * *pos sits on the first payload window), 0 when rejected or out of stream.
 */
static int
sync_frame(struct source *src, struct frame_sync *fs, size_t *pos, struct frame_sync_status *st)
{
    const float complex *w;

    while ((w = source_window(src, *pos, *pos + 2 * (size_t)st->samples_per_symbol)))
    {
        uint32_t advance = 0;
        int ret = frame_sync_sync_step(fs, w, &advance);
        frame_sync_status(fs, st);

        if (ret > 0)
        {
            *pos += advance;
            log_info(
                "sync word ok: netid=%d,%d  cfo=%+.3f bins (%.1f Hz) sfo=%+f  "
                "sto_frac=%+f -> payload grid at sample %zu\n",
                st->netid_dec[0],
                st->netid_dec[1],
                (double)st->cfo,
                (double)st->cfo * st->bandwidth / st->number_of_bins,
                (double)st->sfo_hat,
                (double)st->sto_frac,
                *pos);
            log_input_sample(src, *pos);

            return 1;
        }

        if (ret < 0)
        {
            log_debug(
                "sync failed: netid1=%d netid2=%d (want %d,%d)\n",
                st->netid_dec[0],
                st->netid_dec[1],
                st->sync_words[0],
                st->sync_words[1]);

            return 0;
        }

        *pos += st->samples_per_symbol; // SYNC consumes one window per step
    }
    return 0;
}

static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (4 * ((in_len + 2) / 3) + 1 > out_cap)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len)
            v |= in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = i + 1 < in_len ? tab[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < in_len ? tab[v & 63] : '=';
    }
    out[o] = '\0';
    return 0;
}

// Report one completed frame: the human verdict on stderr, one JSON line on
// stdout. crc_valid carries the verdict: 1 valid, 0 invalid, -1 absent.
static void report_frame(
    const struct frame_result *res, enum frame_decode verdict, const struct frame_sync_status *st)
{
    char b64[384] = {0}; // max 341 used
    struct output_record rec = {0};

    rec.packet = res->bytes;
    rec.len = res->len;
    rec.snr_db = res->snr_db;
    rec.crc_present = res->has_crc;
    rec.crc_valid = verdict == DECODE_CRC_OK ? 1 : (verdict == DECODE_CRC_BAD ? 0 : -1);
    rec.cfo_hz = (double)st->cfo * st->bandwidth / (double)st->number_of_bins;

    if (b64_encode(res->bytes, res->len, b64, sizeof(b64)) != 0)
        b64[0] = '\0'; // one encode serves both the stderr line and stdout
    log_info("RX: %s\n", b64);

    if (!rec.crc_present)
        log_info("CRC absent\n");
    else if (rec.crc_valid)
        log_info("CRC valid!\n");
    else
        log_warn("CRC invalid\n");

    output_frame(&rec, b64);
}

struct receiver *receiver_create(const struct receiver_config *cfg)
{
    struct receiver *r = calloc(1, sizeof(*r));

    if (!r)
        return NULL;

    r->chase = cfg->chase;
    r->report_crc_bad = cfg->report_crc_bad;
    r->fs = frame_sync_create(
        (uint8_t)cfg->sf,
        (uint8_t)cfg->os,
        (uint16_t)cfg->preamble_avg,
        (uint32_t)cfg->sync_word,
        (uint32_t)cfg->center_freq,
        (uint32_t)cfg->bandwidth,
        (uint8_t)cfg->detect_avg);
    if (!r->fs)
    {
        free(r);
        return NULL;
    }

    if (demod_init(&r->demod, (uint8_t)cfg->sf) < 0)
    {
        frame_sync_destroy(r->fs);
        free(r);
        return NULL;
    }

    return r;
}

void receiver_destroy(struct receiver *r)
{
    if (!r)
        return;
    demod_cleanup(&r->demod);
    frame_sync_destroy(r->fs);
    free(r);
}

int receiver_run(struct receiver *r, struct source *src)
{
    struct frame_sync *fs = r->fs;
    struct frame_sync_status st;
    struct frame_result res;

    // Frame detect statistics
    int n_locks = 0, n_synced = 0, n_decoded = 0;
    int n_crc_ok = 0, n_crc_bad = 0, n_crc_absent = 0;
    int n_header_fail = 0, n_truncated = 0;

    frame_sync_status(fs, &st); // one snapshot, refreshed after every step below

    size_t pos = 0;
    size_t fire_pos = 0; // the fire window itself, before the coarse jump
    for (;;)
    {
        size_t frame_pos = pos;
        int detected = detect_frame(src, fs, &pos, &st, &fire_pos);

        if (!detected)
            break; // end of stream: nothing more to hunt
        n_locks++;

        if (sync_frame(src, fs, &pos, &st))
        {
            n_synced++;

            demod_reset(&r->demod, st.cfo);
            enum frame_decode verdict =
                decode_frame(src, fs, &r->demod, &pos, &res, &st, &r->chase);

            switch (verdict)
            {
            case DECODE_CRC_OK:
                n_crc_ok++;
                n_decoded++;
                report_frame(&res, verdict, &st);
                break;
            case DECODE_NO_CRC:
                n_crc_absent++;
                n_decoded++;
                report_frame(&res, verdict, &st);
                break;
            case DECODE_CRC_BAD:
                n_crc_bad++;
                if (r->report_crc_bad)
                    report_frame(&res, verdict, &st);
                break;
            case DECODE_HEADER:
                n_header_fail++;
                break;
            case DECODE_TRUNCATED:
                n_truncated++;
                break;
            }

            /* a bad verdict can hide a later real preamble under the failed
             * decode: rewind past the fire window and rescan (truncated
             * frames have nothing to recover) */
            if (verdict == DECODE_HEADER || verdict == DECODE_CRC_BAD)
                pos = fire_pos + (size_t)REWIND_SKIP_SYMS * st.samples_per_symbol;
        }

        // rewind for the next frame; the guard keeps the scan moving even if
        // a rejected frame consumed nothing
        frame_sync_reset_detect(fs);
        if (pos <= frame_pos)
            pos = frame_pos + st.samples_per_symbol;
        source_advance(src, pos);
    }

    log_info(
        "stats: locks=%d synced=%d decoded=%d crc_ok=%d crc_bad=%d "
        "crc_absent=%d header_fail=%d truncated=%d\n",
        n_locks,
        n_synced,
        n_decoded,
        n_crc_ok,
        n_crc_bad,
        n_crc_absent,
        n_header_fail,
        n_truncated);

    return n_decoded;
}
