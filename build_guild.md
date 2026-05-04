# Hướng dẫn Build NexusKey (Phiên bản mới nhất)

Tài liệu này tổng hợp các bước để cài đặt môi trường và biên dịch (build) dự án NexusKey trên Windows bằng bộ công cụ Visual Studio mới nhất.

## 1. Yêu cầu hệ thống (Prerequisites)

Bạn cần cài đặt các công cụ sau:
*   **Visual Studio Build Tools** (Bản 2022 hoặc 2025 mới nhất).
    *   Link tải: [vs_BuildTools.exe](https://aka.ms/vs/17/release/vs_BuildTools.exe)
    *   Khi cài đặt, chọn gói: **Desktop development with C++**.
    *   Đảm bảo có tích chọn: **C++ CMake tools for Windows**.
*   **CMake**: Nên sử dụng bản đi kèm với Visual Studio.

---

## 2. Cách Build bản Standard (Giao diện hiện đại)

Đây là bản đầy đủ sử dụng Sciter UI.

```powershell
# 1. Tạo thư mục build và cấu hình (Sử dụng trình biên dịch mặc định trên máy)
cmake -B build -A x64

# 2. Biên dịch ứng dụng
cmake --build build --config Release --target NextKeyApp
```

*   **File thực thi**: `build\src\app\Release\NexusKey.exe`
*   **Lưu ý**: Cần copy file `sciter.dll` vào cùng thư mục để chạy.

---

## 3. Cách Build bản Basic (Lite - Giao diện truyền thống)

Bản này sử dụng Win32 native UI, nhẹ hơn và không cần `sciter.dll`.

```powershell
# 1. Xóa thư mục build cũ để cấu hình lại
Remove-Item -Recurse -Force build

# 2. Cấu hình với chế độ LITE_MODE
cmake -B build -DNEXUSKEY_LITE_MODE=ON -A x64

# 3. Biên dịch bản Lite
cmake --build build --config Release --target NextKeyLite
```

*   **File thực thi**: `build\src\app\Release\NexusKeyClassic.exe`

---

## 4. Lưu ý khi thực hiện

*   **Dùng đúng Terminal**: Nên sử dụng **Developer PowerShell for VS 2022/2025** để đảm bảo mọi lệnh build đều được nhận diện chính xác.
*   **Lỗi Toolset**: Nếu bạn gặp lỗi yêu cầu `v143`, hãy đảm bảo không sử dụng tham số `-G "Visual Studio 17 2022"` trong lệnh `cmake`, hãy để CMake tự chọn trình biên dịch mới nhất trên máy bạn bằng lệnh `cmake -B build -A x64`.
