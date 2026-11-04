# Write overhead of constraint enforcement
# 2,000 single-row INSERTs into Purchases (valid shopper ids), one
# process per run, best-of-3 wall time. Same statements in both modes.

| enforcement | run 1 ms | run 2 ms | run 3 ms | best ms | us/insert |
|---|---:|---:|---:|---:|---:|
| on | 91 | 96 | 96 | 91 | 9.10 |
| off | 64 | 66 | 62 | 62 | 6.20 |
