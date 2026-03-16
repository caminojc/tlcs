This directory has integration tests for Opus which should work as is for SMPL
audio codec as well.

## Running Tests Locally
You can compile and run the tests by executing following from `Codec/C` directory:
```
cmake -S . -B build
cmake --build build/test --config Release -j 16
./build/test/opus_smpl_tests
```

It is *required* to run the tests from `Codec/C` directory as the tests read PCM files whose path is relative to `Codec/C` directory.
