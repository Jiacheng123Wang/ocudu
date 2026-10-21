# depth4 — the LDPC-including network

The product code for the next contract. **Empty on purpose**: the design is not settled yet, and code that arrives
before its design is a decision nobody recorded.

What is known so far, and nothing more:

* depth3 ends at soft bits. depth4 extends the network through the **LDPC decode**, so the receiver's output becomes the
  decoded transport block rather than the LLRs.
* The open questions that decide the shape of this directory: whether the network replaces the decoder, predicts
  something the decoder consumes (a schedule, a set of messages, a stopping rule), or wraps it; how a decoded block is
  supervised through a non-differentiable decoder; and what the contract's inputs are once the decode needs the
  LDPC base graph, the lifting size and the redundancy version.
* The working tree for those questions is `doc_chinese/depth4_lpdc_ai/` — `work_tmp/` for experiments, and the
  design record in `depth4_lpdc_ai_design_and_implementation.md`.

When a piece is proven it moves here, and only then. The artifact directory for this contract will be created beside
this README once there is something that could pass a gate.
