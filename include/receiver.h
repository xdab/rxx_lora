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
 * Chase-II payload retry tuning: on a CRC fail, the k least-reliable payload
 * codewords are retried on their runner-up candidates, singles then pairs;
 * every CRC-passing combo is accepted by lowest total flip LLR loss, within
 * the tau budget. Off unless enabled; the CLI's --chase/--chase-k/--chase-tau
 * set it. The suspicion gate (a suspect's margin must be under 0.2 of its
 * LLR mass) is baked in the receiver: measured flat-optimal, no knob.
 */
struct chase_config
{
    int enabled; // run the retry at all
    int k;       // least-reliable codewords considered, 1..8
    float tau;   // max total flip LLR loss to accept, <0 = off
};

/** The defaults behind --chase and the --chase-k/--chase-tau tuning options. */
#define CHASE_CONFIG_DEFAULTS {0, 8, 1.0f}

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
};

struct receiver *receiver_create(const struct receiver_config *cfg);

void receiver_destroy(struct receiver *r);

/** Hunt and decode every frame the source can supply; returns frames decoded. */
int receiver_run(struct receiver *r, struct source *src);

#endif // RECEIVER_H
