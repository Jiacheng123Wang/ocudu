# depth4_lpdc_ai — where this workflow lives

The next step after depth3: extend the network past the soft bits and include the LDPC decode. The layout mirrors
`doc_chinese/llr_ai_detection/` on purpose, because the working habits that tree accumulated are part of the method.

| Path | What belongs here | What does NOT |
|---|---|---|
| `depth4_lpdc_ai_high_level_status_and_plan.md` | The rolling status: what is done, what is next, what is blocked, the pre-registered gates, the milestones. Short, current, edited in place. | Measurements (design document), literature (survey). |
| `depth4_lpdc_ai_design_and_implementation.md` | The design record: decisions with the measurement that justified them, appended section by section, each entry dated (D0, D1, ...). Immutable once written; corrections are new entries that say what they correct. | Status, plans, wish lists, paper summaries. |
| `survey/` | **Literature only**: the question, the selection rule, the method families, one row per paper with what it contributes and where it stops, the conclusions that survive our constraints, the excluded directions with reasons, the reading list. | Measurements of our system, designs, plans. |
| `work_tmp/` | Scratch: code under development, datasets, runs, captures, throwaway scripts. NOT part of the product and not referenced by the build. | Anything the build or a shipped bundle needs. |
| `wip/` | Work in progress that is meant to survive: a probe worth keeping, a half-built harness, a script another section refers to. | Datasets and multi-gigabyte artifacts. |
| `survey/` | Reading: papers, prior art, vendor documentation, with the reading notes that say what was taken from each. | Opinions without a source. |
| `probe/` | Instrumentation and one-shot investigations: the scripts that answer a question, kept so the question can be asked again. | The experiments themselves. |

## The promotion rule (the same one depth3 ran on)

`work_tmp/` is where things are allowed to be ugly. When something is proven — code that the build should use, an
artifact that passed its acceptance gates — it is **promoted**:

* **code** → `lib/phy/upper/ai_signal_processors/depth4/`
* **artifacts** → `lib/phy/upper/ai_signal_processors/depth3/ai_assets/` for depth3-contract models, and the
  equivalent assets directory here once depth4 has its own contract

A model that has not passed its gates stays in `work_tmp/`. That rule is why `ai_assets/` can be read as "what may be
flown", and it is worth more than the tidiness it costs.
