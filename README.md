# GPU Architecture Research: Bulk

**Exploring hardware-assisted memory organization for irregular GPU workloads.**

## About

This project explores a GPU architecture designed to improve the execution efficiency of irregular memory workloads.

Modern GPUs achieve high performance through massive parallelism, allowing thousands of threads to execute concurrently and hide memory latency. However, workloads involving indirect memory accesses, pointer chasing, graph traversal, and sparse data structures can be difficult to execute efficiently.

These workloads often access memory in unpredictable patterns, creating challenges for address translation, memory-level parallelism, and memory-system utilization.

This project investigates whether specialized hardware can reorganize irregular memory requests to make better use of the GPU's existing memory resources.

Our proposed architecture is called **Bulk**.

## Research Motivation

Conventional SIMT architectures are highly effective for regular and coalesced memory accesses. Irregular workloads present a different challenge: memory requests may be scattered across many virtual pages, making translation and memory access less efficient.

Rather than replacing the conventional GPU memory hierarchy, we investigate whether adding a specialized request-organization mechanism can improve how irregular memory operations are processed.

The central research question is:

> Can hardware-assisted organization of irregular memory requests improve GPU performance beyond what a conventional SIMT architecture can achieve with comparable resources?

## Inspiration: DX100

This research is inspired by the paper:

**DX100: Programmable Data Access Accelerator for Indirection**  
Alireza Khadem et al., ISCA 2025.

[Paper (PDF)](https://deepblue.lib.umich.edu/bitstream/handle/2027.42/198624/3695053.3731015.pdf?sequence=1) · [Official DX100 repository](https://github.com/arkhadem/DX100)

DX100 demonstrates how specialized hardware can reorganize irregular memory accesses to expose memory-level parallelism and improve memory-system utilization.

Its central insight is that indirect memory accesses contain opportunities for organization that conventional execution pipelines may not fully exploit.

This inspired us to investigate a related idea in the GPU domain.

The two projects have different architectures and mechanisms. DX100 organizes accesses around physical DRAM rows and columns, while our current Bulk design focuses on organizing GPU memory requests into virtual-page cohorts.

Our goal is to explore how this general principle of hardware-assisted memory organization can be adapted to massively parallel GPU execution.

## Our Approach: Bulk

Bulk is a proposed GPU architecture built around two specialized functions:

- **Dispatch and selection:** Determines whether operations should use conventional SIMT execution or Bulk execution.
- **Page organization:** Groups irregular memory requests by virtual page into cohorts before releasing them into the conventional memory pipeline.

The design retains the conventional GPU memory hierarchy, including caches, TLBs, page walks, outstanding memory requests, and DRAM scheduling.

This allows us to investigate the benefits of specialized request organization without replacing the entire GPU memory system.

## Development

The project is developed using a standalone C++ simulator.

Our work has progressed through several stages:

- Developing a conventional SIMT baseline.
- Establishing resource-matched comparisons for fair architectural evaluation.
- Designing and implementing the Bulk page-organization mechanism.
- Investigating translation, memory-level parallelism, and DRAM behavior.
- Developing workload evaluations and component-level ablations.
- Auditing simulator correctness, resource accounting, and forward progress.
- Evaluating the architecture across a broader range of workloads.
- Investigating how translation resources and TLB reach affect Bulk's performance.

The architecture and its evaluation continue to evolve as we identify the conditions under which hardware-assisted memory organization provides meaningful benefits.

For detailed experiments, results, and design decisions, see the development log and research documentation.

## Project Goals

The long-term goal is to develop and evaluate an architecture that can improve irregular GPU memory execution while preserving the advantages of conventional SIMT processing.

We are particularly interested in understanding:

- When page-level request organization is beneficial.
- How translation pressure influences GPU performance.
- How request organization affects memory-level parallelism.
- Which workloads benefit from specialized hardware.
- What additional hardware resources are required.
- How an effective execution-selection policy should be designed.

We aim to evaluate these questions through reproducible simulation, controlled experiments, and careful architectural analysis.

## Repository

This repository contains the C++ simulator, validation infrastructure, experiment configurations, benchmark data, and research documentation.

See the `docs/` directory for additional technical details and experimental records.

---

**This is an ongoing independent research project.** The objective is not to assume that a specialized architecture is always better, but to understand where it works, why it works, and what trade-offs it introduces.
