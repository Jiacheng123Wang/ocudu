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

## Structure

```txt
docs/
├── .env                     # env file for docker-compose
├── docker-compose.yml       # Docker services for documentation
├── doxygen/                 # Doxygen project
└── README.md                # This file
```

## Docker Services

### Usage

Builds doxygen locally with

```bash
docker compose -f docs/docker-compose.yml up
```

You can select another doxygen target using the environmental variable

```bash
DOXYGEN_TARGET=doxygen-support docker compose -f docs/docker-compose.yml up
```

### Environment Variables

To run the docker-compose, you may need to adjust the variables defined in the .env file.

- `UID`/`GID`: Your user/group IDs for file permissions in Docker
