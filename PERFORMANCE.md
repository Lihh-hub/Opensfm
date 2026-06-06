# OpenSfM Performance Options

This fork includes conservative runtime optimizations for ODM workloads. The
defaults favor processing speed while retaining compatibility with existing
feature, word, match, and track files.

The bundled OpenSfM depthmap path also uses integral-image patch variance and
precomputed bilateral weights. ODM pipelines that use OpenMVS for dense
reconstruction are unaffected by that change.

## Recommended ODM Configuration

```yaml
processes: 8

# Avoid an O(N^2) fallback when GPS, time, BoW, and VLAD selection produce no
# candidates. Set to 0 only when legacy all-pairs matching is required.
matching_default_neighbors: 20
matching_no_gps_neighbors: 20

# Use CPU-cheap intermediate files. COMPRESSED uses less disk space.
intermediate_storage: FAST
matches_gzip_compresslevel: 1

# Ceres defaults used by this fork.
bundle_linear_solver_type: SPARSE_SCHUR
local_bundle_linear_solver_type: DENSE_SCHUR
bundle_shot_poses_linear_solver_type: DENSE_QR
bundle_function_tolerance: 1.0e-6
```

For ordered drone captures, `matching_default_neighbors` between 12 and 30 is
a practical starting range. Increasing it improves tolerance of irregular
capture order but increases feature matching time.

## Native CPU Build

Private workers that build and run on the same CPU can enable native compiler
tuning:

```bash
OPENSFM_CMAKE_ARGS="-DOPENSFM_NATIVE_ARCH=ON" python3 setup.py build
```

AVX VLFeat kernels are enabled by default. For old processors or portable
binary distribution, disable them explicitly:

```bash
OPENSFM_CMAKE_ARGS="-DOPENSFM_ENABLE_AVX=OFF" python3 setup.py build
```

## Compatibility Switches

Use these values to restore the previous storage and matching fallback:

```yaml
matching_default_neighbors: 0
intermediate_storage: COMPRESSED
matches_gzip_compresslevel: 9
```

## Validation

Performance comparisons should use the same image set and record:

- wall time from `features.json`, `matches.json`, `tracks.json`, and
  `reconstruction.json`;
- number of candidate image pairs;
- registered image and reconstructed point counts;
- reprojection and GCP errors;
- final orthophoto completeness.

Reducing feature counts, matching neighbors, bundle iterations, or
retriangulation frequency can provide further speedups, but those settings
should be tuned against reconstruction quality for each camera and flight
pattern.
