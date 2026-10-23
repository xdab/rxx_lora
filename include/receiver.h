#ifndef RECEIVER_H
#define RECEIVER_H

struct source;

/**
 * The generic receive chain: DETECT -> coarse sync -> SYNC -> payload, over
 * and over, for every frame the source can supply. Backend- and
 * input-agnostic; it only ever asks the source for windows.
 */
struct receiver;

struct receiver *receiver_create(
    int sf,
    int os,
    int preamble_avg,
    long sync_word,
    int detect_avg,
    double center_freq,
    double bandwidth);

void receiver_destroy(struct receiver *r);

/** Hunt and decode every frame the source can supply; returns frames decoded. */
int receiver_run(struct receiver *r, struct source *src);

#endif // RECEIVER_H
