import Foundation
import Darwin

private var globalEngineRef: VMEngine?

class VMEngine: ObservableObject {
    @Published var logs: String = "[系統] HengeDebian 完全體 (VHE EL2 + VirtIO + Metal) 已就緒\n"
    @Published var jitEnabled: Bool = false
    @Published var kernelData: Data?
    @Published var diskPath: String = ""

    init() {
        globalEngineRef = self
        checkJIT()
    }

    func checkJIT() {
        typealias CsopsFunc = @convention(c) (pid_t, UInt32, UnsafeMutableRawPointer?, Int) -> Int32
        guard let handle = dlopen(nil, RTLD_NOW), let sym = dlsym(handle, "csops") else { return }
        let csopsCall = unsafeBitCast(sym, to: CsopsFunc.self)
        var flags: UInt32 = 0
        let res = csopsCall(getpid(), 0, &flags, MemoryLayout<UInt32>.size)
        jitEnabled = (res == 0) && ((flags & 0x10000000) != 0)
        logs += jitEnabled ? "[JIT] CS_DEBUGGED 已開啟！原生 ARM64 直跑就緒。\n" : "[JIT] 未開啟 JIT，請用 StikDebug 啟用。\n"
    }

    func loadKernel(url: URL) {
        guard url.startAccessingSecurityScopedResource() else { return }
        defer { url.stopAccessingSecurityScopedResource() }
        if let data = try? Data(contentsOf: url) {
            kernelData = data
            logs += "[內核] 已載入: \(url.lastPathComponent) (\(data.count / 1024) KB)\n"
        }
    }

    func attachDisk(url: URL) {
        guard url.startAccessingSecurityScopedResource() else { return }
        let dst = FileManager.default.temporaryDirectory.appendingPathComponent(url.lastPathComponent)
        try? FileManager.default.removeItem(at: dst)
        if (try? FileManager.default.copyItem(at: url, to: dst)) != nil {
            diskPath = dst.path
            logs += "[硬碟] VirtIO-Blk 已掛載: \(url.lastPathComponent)\n"
        }
        url.stopAccessingSecurityScopedResource()
    }

    func bootRealKernel() {
        guard jitEnabled else { logs += "[錯誤] 請先開啟 JIT！\n"; return }
        guard let data = kernelData else { logs += "[錯誤] 請先匯入 Debian Kernel！\n"; return }
        logs += "[vCPU] 分配 256MB RAM、自動生成 DTB 並啟動 VHE EL2...\n"

        DispatchQueue.global(qos: .userInitiated).async {
            let ok = data.withUnsafeBytes { rawBuf -> Bool in
                guard let ptr = rawBuf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return false }
                return self.diskPath.withCString { dPtr in
                    vm_init_and_load(ptr, UInt64(data.count), dPtr, { c in
                        let str = String(UnicodeScalar(UInt8(bitPattern: c)))
                        DispatchQueue.main.async { globalEngineRef?.logs += str }
                    })
                }
            }
            if ok {
                DispatchQueue.main.async { globalEngineRef?.logs += "[啟動] 跳轉入原生 Kernel 指令流！\n" }
                vm_run_vcpu()
            }
        }
    }
}
