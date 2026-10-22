# Theoretical BER Curves

This document describes the analytical models used to evaluate the end-to-end simulated AWGN performance of our LoRa receiver. 

The models assume non-coherent demodulation over an interleaved AWGN channel, with ideal synchronization. 

These curves serve as theoretical reference points under the stated assumptions, rather than universal performance bounds for every receiver implementation.

## Uncoded BER ($P_b$)

The uncoded LoRa bit error rate uses the compact AWGN symbol error rate approximation discussed in Eq. (18) of [Afisiadis et al. (2019)](https://arxiv.org/abs/1905.11252), expressed here using the simulation's per-sample SNR convention.

Let $M = 2^{\text{SF}}$. The symbol error rate is approximated by

$$
P_s \approx \frac{1}{2}\operatorname{erfc}\left(
\sqrt{M \cdot 10^{\gamma_{\text{dB}}/10}}
-
\sqrt{\ln(2)\cdot\text{SF}+\gamma_{\text{EM}}}
\right).
$$

The corresponding bit error rate is approximated as

$$
P_b = \frac{M}{2(M-1)}P_s
= \frac{2^{\text{SF}-1}}{2^{\text{SF}}-1}P_s.
$$

The conversion assumes that, given a symbol error, the incorrect symbol is approximately uniformly distributed among the other $M-1$ possibilities. This gives an average bit error fraction of $M/[2(M-1)]$, which approaches $1/2$ for large $M$.

### Notation

- **$\gamma_{\text{dB}}$**: Per-complex-sample SNR in dB, under the simulation's normalization. The factor $M$ converts this to the symbol-level SNR term used in the approximation. If the simulation already specifies symbol SNR, this factor must not be applied again.
- **$\text{SF}$**: Spreading factor ($7 \le \text{SF} \le 12$).
- **$\gamma_{\text{EM}}$**: Euler–Mascheroni constant ($\approx 0.57722$).
- **$P_s$**: Uncoded symbol error rate.
- **$M$**: Number of LoRa symbols in the modulation alphabet ($M = 2^{\text{SF}}$).

## Coded BER ($P_{b,\text{coded}}$)

Assuming ideal block interleaving, the bit errors entering each deinterleaved FEC codeword are modeled as independent and identically distributed (iid), with probability $p=P_b$. This assumption follows the interleaving analysis in [Afisiadis et al. (2019), *Coded LoRa Frame Error Rate Analysis*](https://arxiv.org/abs/1911.10245).

The post-decoding BER is calculated by exhaustively evaluating all $2^n$ possible received bit sequences using a hard-decision maximum-likelihood (ML), minimum-distance decoder:

$$
P_{b,\text{coded}} =
\frac{1}{4}
\sum_{w=0}^{2^n-1}
p^{\operatorname{wt}(w)}
(1-p)^{n-\operatorname{wt}(w)}
\cdot
\frac{
\displaystyle\sum_{c\in\mathcal C}
\mathbb I\!\left(d(w,c)=d_{\min}(w)\right)
\operatorname{wt}(u(c))
}{
\displaystyle\sum_{c\in\mathcal C}
\mathbb I\!\left(d(w,c)=d_{\min}(w)\right)
}.
$$

Here, $w$ is interpreted as the received error pattern relative to the all-zero codeword. 

The expression averages the number of incorrectly decoded information bits over all possible patterns, assuming uniform random tie-breaking between equally near codewords. 

The exhaustive BER sum is a project-specific derivation using the iid error model; the cited paper primarily derives codeword error rate (CWER) and frame error rate (FER) approximations.

### Notation

- **$\text{CR}$**: LoRa coding-rate index ($1 \le \text{CR} \le 4$).
- **$n$**: Encoded codeword length ($n=4+\text{CR}$).
- **$\mathcal C$**: Codebook containing the 16 valid encoded codewords of length $n$.
- **$u(c)$**: The original 4-bit information nibble associated with codeword $c$.
- **$\operatorname{wt}(x)$**: Hamming weight, or number of set bits in $x$.
- **$d(w,c)$**: Hamming distance between sequences $w$ and $c$.
- **$d_{\min}(w)$**: Minimum distance from $w$ to any valid codeword, $\min_{c\in\mathcal C}d(w,c)$.
- **$\mathbb I(\cdot)$**: Indicator function, equal to $1$ when its condition is true and $0$ otherwise.
