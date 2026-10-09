# Libraries used for comparison

Header-only C++ caches that the benchmarks compare JrCache with. They are only used by the benchmarks and are copied unmodified from the commits below.

| Folder | Project | Commit | License |
|---|---|---|---|
| `caches/` | [vpetrigo/caches](https://github.com/vpetrigo/caches): LRU, LFU and FIFO policies, always locks a mutex | [`394c274`](https://github.com/vpetrigo/caches/tree/394c2740ebe7f9b67464766478855d0988c2f32e) (2026-07-01) | BSD-3-Clause, see `caches/LICENSE.md` |
| `cpp-lru-cache/` | [lamerman/cpp-lru-cache](https://github.com/lamerman/cpp-lru-cache): minimal LRU, not thread-safe | [`de1c4a0`](https://github.com/lamerman/cpp-lru-cache/tree/de1c4a03569bf3bd540e7f55ab5c2961411dbe22) (2017-05-23) | BSD-3-Clause, see `cpp-lru-cache/LICENSE` |
| `lrucache11/` | [mohaps/lrucache11](https://github.com/mohaps/lrucache11): LRU with an optional lock type | [`130bfd5`](https://github.com/mohaps/lrucache11/tree/130bfd5d745c6286fe7ec769710c399ea31e50b1) (2024-09-11) | ISC-style, in the header of `LRUCache11.hpp` |

Google Benchmark itself is not stored here: `tests/bench/CMakeLists.txt` uses an installed copy if CMake can find one, or downloads release v1.9.5 (checked against its SHA-256).
