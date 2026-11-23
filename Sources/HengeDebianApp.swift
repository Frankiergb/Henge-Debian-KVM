import SwiftUI

@main
struct HengeDebianApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    @StateObject private var vm = VMEngine()

    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text("HengeDebian (EL2 KVM)")
                    .font(.headline)
                Spacer()
                Text(vm.jitEnabled ? "JIT 已開啟" : "未開 JIT")
                    .font(.caption.bold())
                    .padding(6)
                    .background(vm.jitEnabled ? Color.green.opacity(0.2) : Color.red.opacity(0.2))
                    .foregroundColor(vm.jitEnabled ? .green : .red)
                    .cornerRadius(6)
            }
            .padding(.horizontal)

            ScrollView {
                Text(vm.logs)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundColor(.green)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
            }
            .background(Color.black)
            .cornerRadius(8)
            .padding(.horizontal)

            Button("測試啟動 VHE EL2 引擎") {
                vm.bootTest()
            }
            .buttonStyle(.borderedProminent)
            .padding(.bottom)
        }
        .padding(.top)
    }
}
