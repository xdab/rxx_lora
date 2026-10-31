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
#define MAX_BYTES 255    // the PHY length field is one byte

struct receiver
{
    struct frame_sync *fs;
    struct demod_state demod; // allocated once, reset per frame
    int sf;
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
    uint8_t cr;   // coding rate 4/(4+cr), 1..4
    float snr_db; // header symbols' mean on-channel SNR
};

/*
 * The chain names absolute positions in its working grid (bw*os); a capture
 * reader works in the file's own samples. Emit that mapping on a trace line
 * beside each absolute-position report.
 */
static void log_input_sample(struct source *src, size_t pos)
{
    log_debug(
        "input sample %zu (%.3f s)\n",
        source_input_sample(src, pos),
        source_time_seconds(src, pos));
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
    const float complex *out = w ? frame_sync_output_step(fs, w, &advance, st) : NULL;

    if (!out)
        return -1;

    (void)demod_step(fd, out); // hard value unused: soft only
    demod_soft_llrs(fd, llr);
    *pos += advance;

    return 0;
}

/*
 * Deinterleave one block and soft-decode its codewords: the single
 * block-LLRs -> nibbles seam. Header: 8 symbols, sf-2 codewords, cr 4.
 * Payload: cr+4 symbols, sf codewords, the frame's cr.
 *
 * alt/marg optionally receive each codeword's runner-up candidate and
 * best-minus-second score (chase-retry fuel); NULL skips.
 *
 * @return the number of nibbles written, or -1 on a malformed block size
 */
static int decode_block(
    const float (*sym_llr)[MAX_SF],
    int n_sym,
    int n_cw,
    int cr,
    uint8_t *nib,
    uint8_t *alt,
    float *marg)
{
    float cw_llr[MAX_SF][MAX_SF];
    int i;

    if (n_sym < 1 || n_sym > HEADER_SYMBOLS || n_cw < 1 || n_cw > MAX_SF || cr < 1 || cr > 4)
        return -1;

    deinterleave_soft(sym_llr, (uint8_t)n_sym, (uint8_t)n_cw, cw_llr);

    for (i = 0; i < n_cw; i++)
        nib[i] = hamming_decode_soft(
            cw_llr[i], (uint8_t)cr, alt ? &alt[i] : NULL, marg ? &marg[i] : NULL);

    return n_cw;
}

/*
 * Assemble a completed frame: dewhiten the payload nibbles, then the CRC
 * verdict. Pure data in, verdict + bytes out — no logging.
 */
static enum frame_decode finish_frame(
    const uint8_t *nibs, uint32_t nnib, const struct lora_header *h, struct frame_result *out)
{
    uint32_t p;
    uint16_t crc_rx, crc_calc;

    if (nnib < 2u * h->pay_len + (h->has_crc ? 4u : 0u))
        return DECODE_TRUNCATED;

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

static int chase_k(void)
{
    const char *e = getenv("CHASE_K");
    int k = e ? atoi(e) : 4;
    return k > 8 ? 8 : (k < 0 ? 0 : k);
}

/*
 * Chase-II retry, CRC-arbitrated: flip the K least-reliable payload nibbles
 * to their runner-up codeword candidate, singles first, then pairs. A wrong
 * combo passes the CRC with p ~ 2^-16, so K <= 8 keeps false positives
 * negligible. Only payload nibbles flip: the CRC nibbles are the check itself.
 *
 * @return 1 on a rescued frame (out filled), 0 if no combo verified
 */
static int chase_retry(
    const uint8_t *nibs,
    const uint8_t *alt,
    const float *marg,
    uint32_t nnib,
    uint32_t pay_nib,
    const struct lora_header *h,
    struct frame_result *out)
{
    uint8_t tmp[2 * MAX_BYTES + 4], used[2 * MAX_BYTES] = {0};
    uint32_t sel[8];
    struct frame_result res; // scratch: attempts must not clobber the verdict
    int k = chase_k(), n_sel = 0, s, t;

    if (k < 1 || pay_nib == 0 || pay_nib > nnib)
        return 0;
    if (pay_nib < (uint32_t)k)
        k = (int)pay_nib;

    // partial selection: the k payload nibbles with the smallest margins
    for (s = 0; s < k; s++)
    {
        uint32_t best = pay_nib;
        for (uint32_t i = 0; i < pay_nib; i++)
            if (!used[i] && (best == pay_nib || marg[i] < marg[best]))
                best = i;
        if (best == pay_nib)
            break;
        used[best] = 1;
        sel[n_sel++] = best;
    }

    for (s = 0; s < n_sel; s++)
        for (t = -1; t < s; t++) // t = -1: the single flip of sel[s]
        {
            memcpy(tmp, nibs, nnib);
            tmp[sel[s]] = alt[sel[s]];
            if (t >= 0)
                tmp[sel[t]] = alt[sel[t]];
            if (finish_frame(tmp, nnib, h, &res) == DECODE_CRC_OK)
            {
                *out = res;
                log_info(
                    "chase: rescued with %d flipped codeword%s\n",
                    t >= 0 ? 2 : 1,
                    t >= 0 ? "s" : "");
                return 1;
            }
        }
    return 0;
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
    struct frame_sync_status *st)
{
    float sllr[HEADER_SYMBOLS][MAX_SF];
    uint8_t hnib[MAX_SF], nibs[2 * MAX_BYTES + 4];
    uint8_t nalt[2 * MAX_BYTES + 4]; // chase-retry fuel: runner-up candidate
    float nmarg[2 * MAX_BYTES + 4];  // and margin per payload codeword
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

    if (decode_block(sllr, HEADER_SYMBOLS, sf - 2, 4, hnib, NULL, NULL) < 0)
        return DECODE_HEADER;

    if (decode_header(hnib, &h) != 0)
    {
        log_warn("header checksum invalid or empty frame\n");
        return DECODE_HEADER;
    }

    out->cr = h.cr;

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

    // payload blocks of cr+4 symbols -> sf nibbles each, soft-decoded
    cw_len = (uint8_t)(h.cr + 4);
    needed = 2u * h.pay_len + (h.has_crc ? 4u : 0u);
    while (nnib < needed)
    {
        uint8_t pnib[MAX_SF], palt[MAX_SF];
        float pmarg[MAX_SF];
        int n;

        if (next_symbol(src, fs, fd, pos, sllr[nb], st) < 0)
            break; // stream spent or budget spent
        if (++nb < cw_len)
            continue;

        n = decode_block(sllr, cw_len, sf, h.cr, pnib, palt, pmarg);
        if (n < 0)
            return DECODE_HEADER;
        for (i = 0; i < (uint32_t)n && nnib < needed; i++)
        {
            nibs[nnib] = pnib[i];
            nalt[nnib] = palt[i];
            nmarg[nnib] = pmarg[i];
            nnib++;
        }
        nb = 0;
    }
    if (nnib < needed)
    {
        log_warn("frame ended after %u of %u payload nibbles\n", nnib, needed);
        return DECODE_TRUNCATED;
    }

    enum frame_decode verdict = finish_frame(nibs, nnib, &h, out);
    if (verdict == DECODE_CRC_BAD && chase_retry(nibs, nalt, nmarg, nnib, 2u * h.pay_len, &h, out))
        verdict = DECODE_CRC_OK;
    return verdict;
}

/*
 * Hunt the preamble one window at a time; on a fire, advance to the first
 * network-id symbol and return 1. Returns 0 at end of stream, -1 on error.
 * The look-ahead source_advance() keeps the sync word's history readable.
 */
static int
detect_frame(struct source *src, struct frame_sync *fs, size_t *pos, struct frame_sync_status *st)
{
    const float complex *w;

    while ((w = source_window(src, *pos, *pos + st->samples_per_symbol)))
    {
        int64_t advance = 0;
        int ret = frame_sync_detect_step(fs, w, &advance, st);

        if (ret < 0)
        {
            log_error("detect step failed at %zu\n", *pos);
            log_input_sample(src, *pos);
            return -1;
        }

        if (ret > 0)
        {
            /* advance = os*(N - k_hat) + (lead-1)*sps; negative when the
             * sync word starts inside the fire window, clamped at 0 as in
             * the reference */
            int64_t next = (int64_t)*pos + advance;

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
        int ret = frame_sync_sync_step(fs, w, &advance, st);

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

            int grid_shift = (int)advance - (int)(st->samples_per_symbol / 4);
            log_trace(
                "CFO: cfo=%+.4f bins (%.1f Hz) from SFD down_val=%d\n"
                "CFO: payload grid shift = %d samples (= %+.4f chips, "
                "1 chip per bin)\n",
                (double)st->cfo,
                (double)st->cfo * st->bandwidth / st->number_of_bins,
                st->down_val,
                grid_shift,
                (double)grid_shift / (double)st->os);

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

        log_trace(
            "sync step pos=%zu (input %zu) cnt=%d bin=%d\n",
            *pos,
            source_input_sample(src, *pos),
            st->symbol_cnt,
            st->bin_idx);
        *pos += st->samples_per_symbol; // SYNC consumes one window per step
    }
    return 0;
}

// Report one completed frame: the human verdict on stderr, one JSON line on
// stdout. crc_valid carries the verdict: 1 valid, 0 invalid, -1 absent.
static void report_frame(
    const struct frame_result *res,
    enum frame_decode verdict,
    const struct frame_sync_status *st,
    struct source *src,
    size_t pos)
{
    char b64[384] = {0}; // max 341 used
    struct output_record rec = {0};

    rec.packet = res->bytes;
    rec.len = res->len;
    rec.snr_db = res->snr_db;
    rec.cr = res->cr;
    rec.crc_present = res->has_crc;
    rec.crc_valid = verdict == DECODE_CRC_OK ? 1 : (verdict == DECODE_CRC_BAD ? 0 : -1);
    rec.net0 = st->netid_dec[0];
    rec.net1 = st->netid_dec[1];
    rec.sync_word = st->sync_word_rx;
    rec.cfo_hz = (double)st->cfo * st->bandwidth / (double)st->number_of_bins;
    rec.sfo = (double)st->sfo_hat;
    rec.sto_frac = (double)st->sto_frac;
    rec.input_sample = source_input_sample(src, pos);
    rec.time_seconds = source_time_seconds(src, pos);

    if (b64_encode(res->bytes, res->len, b64, sizeof(b64)) == 0)
        log_info("RX: %s\n", b64);

    if (!rec.crc_present)
        log_info("CRC absent\n");
    else if (rec.crc_valid)
        log_info("CRC valid!\n");
    else
        log_warn("CRC invalid\n");

    output_frame(&rec);
}

struct receiver *receiver_create(
    int sf,
    int os,
    int preamble_avg,
    long sync_word,
    int detect_avg,
    double center_freq,
    double bandwidth)
{
    struct receiver *r = calloc(1, sizeof(*r));

    if (!r)
        return NULL;

    r->sf = sf;
    r->fs = frame_sync_create(
        (uint8_t)sf,
        (uint8_t)os,
        (uint16_t)preamble_avg,
        (uint32_t)sync_word,
        (uint32_t)center_freq,
        (uint32_t)bandwidth,
        (uint8_t)detect_avg);
    if (!r->fs)
    {
        free(r);
        return NULL;
    }

    if (demod_init(&r->demod, (uint8_t)sf) < 0)
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

    frame_sync_status(fs, &st); // one snapshot, kept fresh by every step below

    size_t pos = 0;
    size_t lock_pos = 0; // frame start: where DETECT fired
    for (;;)
    {
        size_t frame_pos = pos;
        int detected = detect_frame(src, fs, &pos, &st);

        if (detected < 0)
            goto done;
        if (!detected)
            break; // end of stream: nothing more to hunt
        n_locks++;
        lock_pos = pos;

        if (sync_frame(src, fs, &pos, &st))
        {
            n_synced++;

            demod_reset(&r->demod, st.cfo);
            enum frame_decode verdict = decode_frame(src, fs, &r->demod, &pos, &res, &st);

            switch (verdict)
            {
            case DECODE_CRC_OK:
                n_crc_ok++;
                n_decoded++;
                report_frame(&res, verdict, &st, src, lock_pos);
                break;
            case DECODE_NO_CRC:
                n_crc_absent++;
                n_decoded++;
                report_frame(&res, verdict, &st, src, lock_pos);
                break;
            case DECODE_CRC_BAD:
                n_crc_bad++;
                report_frame(&res, verdict, &st, src, lock_pos);
                break;
            case DECODE_HEADER:
                n_header_fail++;
                break;
            case DECODE_TRUNCATED:
                n_truncated++;
                break;
            }
        }

        // rewind for the next frame; the guard keeps the scan moving even if
        // a rejected frame consumed nothing
        frame_sync_reset_detect(fs);
        if (pos <= frame_pos)
            pos = frame_pos + st.samples_per_symbol;
        source_advance(src, pos);
    }

done:
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
