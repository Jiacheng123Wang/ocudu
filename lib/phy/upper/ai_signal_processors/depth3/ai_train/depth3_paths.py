"""Where the depth-3 package finds what it does not own.

The depth-3 receiver's training code lives here, under `ai_signal_processors/depth3/ai_train/`, because
the depth-3 arm IS the estimator, the equaliser and the demapper in one unit (design section 19.226). It
still uses a handful of DSP utilities that the CHANNEL ESTIMATOR's training tree owns -- the DM-RS
sequence generator, the TDL channel synthesiser, the label builder and the LDPC decode helper -- because
those are shared with the AI-CE work and moving them would touch that tree.

One file knows that, so moving them later is a one-line change here instead of an edit in nine scripts.
Every depth-3 training script calls `add_ce_shared()` before importing any of them.
"""
import os
import sys

#: The AI-CE training tree, which still owns the shared DSP utilities.
CE_SHARED = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "signal_processors", "channel_estimator", "metal", "ai_train"))

#: Modules imported from there. Kept as data so a missing one names itself.
SHARED_MODULES = ("dmrs", "synth", "dd_label", "ldpc_decode", "nr_ldpc", "build_labels")


def add_ce_shared():
    """Put the AI-CE training tree on sys.path, once, and say so if it is not there."""
    if not os.path.isdir(CE_SHARED):
        raise ImportError(f"the AI-CE training tree is not at {CE_SHARED}; depth3/ai_train needs "
                          f"{', '.join(SHARED_MODULES)} from it (see depth3_paths.py)")
    if CE_SHARED not in sys.path:
        sys.path.insert(0, CE_SHARED)
    return CE_SHARED
