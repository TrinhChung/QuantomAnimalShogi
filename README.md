# Quantum Animal Shogi

Engine C++17 cho Quantum Animal Shogi. Repo gồm engine `qas`, test tự động và
các benchmark hiệu năng/search.

## Yêu cầu

- CMake 3.15+
- Trình biên dịch C++17
- Windows PowerShell hoặc terminal tương đương

## Build

Tạo build folder Release:

```powershell
cmake -S . -B build_stage5 -DCMAKE_BUILD_TYPE=Release
```

Build engine chính:

```powershell
cmake --build build_stage5 --target qas --config Release
```

File sau khi build:

```text
build_stage5\Release\qas.exe
```

Build toàn bộ target:

```powershell
cmake --build build_stage5 --config Release
```

## Test

Chạy toàn bộ test:

```powershell
ctest --test-dir build_stage5 -C Release --output-on-failure
```

Chạy riêng search test:

```powershell
ctest --test-dir build_stage5 -C Release -R alpha_beta --output-on-failure
```

## Chạy engine

Chạy protocol mặc định:

```powershell
build_stage5\Release\qas.exe
```

Chạy search từ file state:

```powershell
Get-Content state.txt | build_stage5\Release\qas.exe search 1000 8
```

## Benchmark

Build benchmark chính:

```powershell
cmake --build build_stage5 --target qas_stage55_ceiling --config Release
```

Benchmark nhanh:

```powershell
build_stage5\Release\qas_stage55_ceiling.exe --mode quick --out-dir local_reports\quick
```

Stage 5.6 ceiling/overnight style:

```powershell
build_stage5\Release\qas_stage55_ceiling.exe --mode all --out-dir local_reports\stage56 --runs 3 --timeout-ms 60000 --tt-mb 512
```

Stage 5.7 ordering audit:

```powershell
build_stage5\Release\qas_stage55_ceiling.exe --mode ordering --out-dir local_reports\stage57_ordering --runs 3 --timeout-ms 60000 --tt-mb 512
```

Các file CSV benchmark sẽ nằm trong thư mục truyền qua `--out-dir`.

## Artifact tiện dùng

Nếu cần copy binary ra thư mục artifact:

```powershell
New-Item -ItemType Directory -Force -Path local_reports\build_artifacts | Out-Null
Copy-Item build_stage5\Release\qas.exe local_reports\build_artifacts\qas.exe -Force
```

## Ghi chú

- Không ghi log/protocol phụ ra stdout ngoài output chính thức.
- Diagnostics nên ghi ra stderr hoặc file report.
- Sau khi sửa C++, format file đã đổi bằng `clang-format`.
