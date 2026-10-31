## The core

1. **Segment before you theorize.** An aggregate rate (error rate, SNR, pass %) mixes mechanisms with different cures. Split it by ground-truth category (input type, regime, stage) _before_ forming a story. _Action: build the per-case table first, then look at the average._

2. **Treat every inherited belief as a hypothesis with an expiry date.** This includes your own notes, docs, and prior conclusions. _Action: when a belief drives a decision, re-measure it or mark it unverified._

3. **Every test needs a way to fail.** If the pass criterion shares code, data, or bias with the thing under test, agreement is guaranteed. _Action: ask "what result would make this say no?" If you can't name one, the test is decoration. Use an independent reference path._

4. **When a result is impossible, debug the instrument first.** Analysis scripts, plotting, unit conversions, and off-by-one alignment are far likelier culprits than the system. _Action: make it a ritual: check the measuring code before the measured thing._

5. **Quote the number that matches the claim.** If you say "works on real data," don't cite the synthetic-data metric. _Action: state the definition and population next to every number._

## Experiment design

6. **Capture once, query many.** Log raw inputs, raw outputs, and per-case ground truth in a single run so later questions are lookups. This one is a huge time saver.

7. **Run the discriminating experiment before building the fix.** When two explanations compete, find the cheapest test where they predict different outcomes. It also tells you which fix to do first.

8. **Pair your comparisons.** Same seeds, same inputs for every variant. If a confound can ride along with the change, add an arm that isolates it.

9. **Isolate a stage by giving it perfect inputs.** This tells you what the stage contributes. Also check what it was actually _offered_, since the failure often isn't its fault.

10. **Your test setup is part of the experiment.** Unrealistic edges (no warm-up, hard truncation, clean silence before the signal) create phantom effects. _DSP example: filter transients and windowing artifacts at the boundaries get mistaken for algorithm behavior._

11. **Treat expert advice as predictions.** Extract what the claim implies, run the cheapest check, and let data decide. Same applies to code review and AI suggestions.

## Pipeline and estimator design

12. **Diagnose "wrong choice" vs. "right answer never offered."** In multi-stage systems (candidate generation then scoring, detection then classification), these have opposite cures. Measure candidate-set recall _before_ tuning any scorer; no downstream refinement can recover what upstream dropped.

13. **If the observable is blind to the quantity, scoring can't find it.** Stop tuning the metric and add a different measurement or an absolute reference. _Example: a magnitude-only criterion can't resolve phase._

14. **Combine scores in one unit, normalized against one common reference.** Per-candidate normalization turns noise into votes.

15. **Re-derive after any change of frame.** Retiming, resampling, renormalizing, and rebasing all invalidate formulas derived under the old frame. _Action: after such a change, list which formulas assumed the old frame._

## Integration and shipping

16. **Match the neighbors' conventions, not the textbook.** A "better" replacement that violates the interface assumptions of surrounding code (scaling, sign, latency, indexing) is a regression.

17. **Test the extremes, including the trivial case.** Where the system _should_ be perfect (clean input, zero noise, identity config), any imperfection reveals a structural bug. Also test configurations the prototype never saw.

18. **Build instruments early.** Plots, dumps, and annotated views are how you see the problem; they aren't documentation.

19. **Experiment on copies.** Keep the shipped artifact untouched; variants live in scratch space.
