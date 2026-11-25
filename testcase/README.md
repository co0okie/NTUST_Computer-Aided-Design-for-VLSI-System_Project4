# testcase/

The submitted test case for requirement 5: a circuit where `-h` and `-e` give different
latencies. This file is for the repository only; at submission the same information goes into
`ReadMe`.

| File | Constraints (AND OR NOT) | `-h` | `-e` | Verified with |
|---|---|---|---|---|
| `claude_gap4.blif` | 3 3 3 | 26 | 25 (optimal) | branch `claude` @ 380b8dd |

`claude_gap4.blif` (180 operations: 63 AND, 59 OR, 58 NOT; depth 20) was generated on branch
`claude` with

```
python3 benchmark/hard/gen_hard.py layered seed=9914 layers=20 width=9 profile=uniform \
    phased=1 fanmax=3 back=4 -o benchmark/claude_gap4.blif
```

The difference depends on the heuristic: a stronger `-h` may close it. Re-verify with the final
program before submitting.
