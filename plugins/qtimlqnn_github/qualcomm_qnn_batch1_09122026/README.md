# Qualcomm QNN Batch 1 reference patches

Source package: `qnn_batch1_09122026`, received 2026-09-14.

This project semantically ports only patch `0002` into the independently
compiled `qtimlqnn_github` plug-in. The target model is a QNN context binary,
so patch `0001` (DLC graph loading) is intentionally not applied. Patches
`0003` and `0004` were already represented by the local source baseline and
were not reapplied. The vendor prebuilt shared object is retained outside this
repository and is not installed or used by the A/B runner.
