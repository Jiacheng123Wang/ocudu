# OCUDU Documentation

This directory contains the automated API documentation generation (Doxygen) for OCUDU code,
together with the hand-written English documents listed below.

## Documents

| Document | Content |
|---|---|
| [`build_macOS_note_english.md`](build_macOS_note_english.md) | Dependencies and step-by-step build guide for a fresh macOS (Apple Silicon), including the Metal GPU toolchain |
| [`apple_silicon_heterogeneous_gnb_plan_english.md`](apple_silicon_heterogeneous_gnb_plan_english.md) | Long-term plan for the Apple Silicon heterogeneous gNB (Metal GPU / performance cores / NPU) |
| [`nvidia_cuda_build.md`](nvidia_cuda_build.md) | Building with the NVIDIA CUDA acceleration path |

The Chinese counterparts of the first two documents are kept outside the git tree in
`doc_chinese/` (see `.gitignore`).

## Contents

- [agents/](agents/README.md): reference documentation aimed at AI coding agents.
- [doxygen/](doxygen/README.md): Doxygen project and the Docker Compose setup that builds the API documentation.
- [nvidia_cuda_build.md](nvidia_cuda_build.md): build prerequisites and CMake options for CUDA acceleration.
