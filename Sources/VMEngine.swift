import Foundation
import Darwin

private var globalEngineRef: VMEngine?

class VMEngine: ObservableObject {
    @Published var logs: String = "[系統] HengeDebian VHE EL2 引擎已就緒\n"
    @Published var jitEnabled: Bool = false
    @Published var kernelData: Data?

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
        logs += jitEnabled ? "[JIT] CS_DEBUGGED 已開啟！可原生直跑機器碼。\n" : "[JIT] 未開啟 JIT，請用 StikDebug 啟用。\n"
    }

    func loadKernel(url: URL) {
        guard url.startAccessingSecurityScopedResource() else { return }
        defer { url.stopAccessingSecurityScopedResource() }
        if let data = try? Data(contentsOf: url) {
            kernelData = data
            logs += "[載入] 成功讀取 Kernel: \(url.lastPathComponent) (\(data.count / 1024) KB)\n"
        }
    }

    func bootRealKernel() {
        guard jitEnabled else { logs += "[錯誤] 必須先開啟 JIT 先可以執行原生機器碼！\n"; return }
        guard let data = kernelData else { logs += "[錯誤] 請先點擊「匯入 Debian Kernel」揀選檔案！\n"; return }
        logs += "[vCPU] 正在將 Kernel 載入 0x40000000 並偽裝為 VHE EL2...\n"

        DispatchQueue.global(qos: .userInitiated).async {
            let ok = data.withUnsafeBytes { rawBuf -> Bool in
                guard let ptr = rawBuf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return false }
                return vm_init_and_load(ptr, UInt64(data.count), { c in
                    let str = String(UnicodeScalar(UInt8(bitPattern: c)))
                    DispatchQueue.main.async { globalEngineRef?.logs += str }
                })
            }
            if ok {
                DispatchQueue.main.async { globalEngineRef?.logs += "[vCPU] 跳轉進入原生 ARM64 Kernel 指令流！\n" }
                vm_run_vcpu()
            } else {
                DispatchQueue.main.async { globalEngineRef?.logs += "[錯誤] mmap 申請記憶體失敗\n" }
            }
        }
    }
}
