#ifndef RECEIVER_H
#define RECEIVER_H

struct source;

/**
 * The generic receive chain: DETECT -> coarse sync -> SYNC -> payload, over
 * and over, for every frame the source can supply. Backend- and
 * input-agnostic; it only ever asks the source for windows.
 */
struct receiver;

/**
 * Chase retry: on a CRC fail, the 8 least-reliable payload codewords are
 * retried on their runner-up candidates, singles then pairs; every
 * CRC-passing combo is accepted by lowest total flip LLR loss, within the
 * loss budget. Off unless enabled (--chase).
 */
struct chase_config
{
    int enabled; // run the retry at all
};

/** Chase retry off. */
#define CHASE_CONFIG_DEFAULTS {0}

struct receiver_config
{
    int sf;
    int os;
    int preamble_avg;
    long sync_word;
    int detect_avg;
    double center_freq;
    double bandwidth;
    struct chase_config chase;
    int report_crc_bad;
};

struct receiver *receiver_create(const struct receiver_config *cfg);

void receiver_destroy(struct receiver *r);

/** Hunt and decode every frame the source can supply; returns frames decoded. */
int receiver_run(struct receiver *r, struct source *src);

#endif // RECEIVER_H
