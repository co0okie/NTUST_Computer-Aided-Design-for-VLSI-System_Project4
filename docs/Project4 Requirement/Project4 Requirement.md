**Latest update: 11/19, 16:00**

**Assignment requirements:** Your program will be built and evaluated in a Linux environment (e.g., WSL). GCC with C++17 is recommended. Each run has a time limit of 60 seconds.

The `-e` mode **must use Gurobi**; other ILP solvers are not allowed. Gurobi is much faster than open-source solvers such as GLPK, which may time out on larger test cases.

Your `CMakeLists.txt` must locate Gurobi through the `GUROBI_HOME` environment variable (e.g. `$ENV{GUROBI_HOME}/include`, `$ENV{GUROBI_HOME}/lib`). Do not hard-code absolute paths or version-specific library names such as `libgurobi120.so`.

**Build and execution procedure:** Your program will be built and run as follows:

```sh
cd M123123123
cmake -B build
cmake --build build
cd build
# BLIF files are copied into build/
./mlrcs -h sample02.blif 2 1 1
./mlrcs -e sample02.blif 2 1 1
```

The compiled program must be located at `build/mlrcs` (not `build/src/mlrcs` or anywhere else). All input files are placed in `build/`, and all output files must be written to `build/`, i.e., the current working directory.

**Output files:** Only the output files are graded; stdout is ignored.

- `-h` writes `<BLIF base name>_h.out`, e.g. `sample02_h.out`
- `-e` writes `<BLIF base name>_e.out`, e.g. `sample02_e.out`

The file content must follow the format in Project04.pdf exactly:

```text
Heuristic Scheduling Result
1: {h} {j} {i}
2: {l m} {g} {}
3: {n} {k} {p}
4: {o} {} {q}
LATENCY: 4
END
```

For `-e`, the first line is `ILP-based Scheduling Result`.

- Each line is `cycle: {AND ops} {OR ops} {NOT ops}`.
- Node names inside braces are separated by single spaces, and an empty set is written `{}`.

**Required directory structure:** After building and running, the project should look like this:

```text
M123123123/
    build/
        mlrcs             # compiled program
        sample02.blif     # input (copied into build/)
        sample02_h.out    # output
        sample02_e.out
    src/                  # source code
    testcase/             # your own test case (requirement 5)
    CMakeLists.txt
    ReadMe
```

At submission, the project should look like this:

```text
M123123123/
    src/
    testcase/
    CMakeLists.txt
    ReadMe
```

## Submission notes:

1. Do not upload the `build/` directory.
2. The project directory must be named after your student ID and compressed as `StudentID.tgz`, e.g. `M11515013.tgz` (`tar -czf M11515013.tgz M11515013`).
3. Put the test case you made for requirement 5 (where `-h` and `-e` give different latencies) in `testcase/`. In the ReadMe, state which resource constraints to run it with.
4. Submit the report as a PDF, at most 3 pages. `.md` and `.docx` are not accepted. Name it `StudentID.pdf` and upload it together with `StudentID.tgz`. Do not put the report inside the tarball.
5. The compiled program must be named `mlrcs`. You can set the name in `CMakeLists.txt`.
6. Usage: `mlrcs -h/-e BLIF_FILE AND_CONSTRAINT OR_CONSTRAINT NOT_CONSTRAINT`. If arguments are missing or invalid, the program must print an error message.
7. No additional test cases will be provided. Use `sample02.blif` from Project04.pdf and create your own test cases.
8. **Grading:** Your program will be evaluated on the following:

   - whether the `-e` result is optimal
   - whether the `-h` schedule is legal
   - how close the `-h` latency is to the optimal latency from `-e`
   - the report

The exact weighting is not finalized yet, and part of the `-h` score may be based on ranking among students. Make sure your schedules are legal first, since this matters most. After that, work on improving the latency of your heuristic.
