import Foundation
import Darwin

@_silgen_name("csops")
func csops(_ pid: pid_t, _ ops: UInt32, _ useraddr: UnsafeMutableRawPointer?, _ usersize: Int) -> Int32

class VMEngine: ObservableObject {
    @Published var logs: String = "[系統] HengeDebian VHE EL2 引擎已就緒\n"
    @Published var jitEnabled: Bool = false

    init() {
        checkJIT()
    }

    func checkJIT() {
        var flags: UInt32 = 0
        let CS_OPS_STATUS: UInt32 = 0
        let CS_DEBUGGED: UInt32 = 0x10000000
        let res = csops(getpid(), CS_OPS_STATUS, &flags, MemoryLayout<UInt32>.size)
        jitEnabled = (res == 0) && ((flags & CS_DEBUGGED) != 0)
        logs += jitEnabled ? "[JIT] 成功檢測到 CS_DEBUGGED 權限！可進行原生直跑。\n" : "[JIT] 未開啟 JIT，請透過 StikDebug / SideStore 啟用。\n"
    }

    func bootTest() {
        logs += "[vCPU] 初始化 ARMv8.1 VHE EL2 虛擬暫存器 (CurrentEL = 2)...\n"
        logs += "[UART] 0x09000000 PL011 串口監聽中，請匯入 Debian Image 開始啟動。\n"
    }
}
