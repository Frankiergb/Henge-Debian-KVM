import SwiftUI
import UniformTypeIdentifiers

@main
struct HengeDebianApp: App {
    var body: some Scene { WindowGroup { ContentView() } }
}

struct ContentView: View {
    @StateObject private var vm = VMEngine()
    @State private var pickerMode = 0 // 1=Kernel, 2=Disk
    @State private var showPicker = false
    @State private var showMetal = false

    var body: some View {
        VStack(spacing: 10) {
            HStack {
                Text("HengeDebian KVM").font(.headline)
                Spacer()
                Button(showMetal ? "切去終端機" : "Metal GPU 畫面") { showMetal.toggle() }
                    .font(.caption).buttonStyle(.bordered)
                Text(vm.jitEnabled ? "JIT ON" : "NO JIT")
                    .font(.caption.bold()).padding(5)
                    .background(vm.jitEnabled ? Color.green.opacity(0.2) : Color.red.opacity(0.2))
                    .foregroundColor(vm.jitEnabled ? .green : .red).cornerRadius(6)
            }.padding(.horizontal)

            if showMetal {
                MetalScreenView().cornerRadius(8).padding(.horizontal)
            } else {
                ScrollView {
                    Text(vm.logs).font(.system(.caption, design: .monospaced))
                        .foregroundColor(.green).frame(maxWidth: .infinity, alignment: .leading).padding(10)
                }.background(Color.black).cornerRadius(8).padding(.horizontal)
            }

            HStack(spacing: 8) {
                Button("1.匯入Kernel") { pickerMode = 1; showPicker = true }.buttonStyle(.bordered)
                Button("2.掛載硬碟") { pickerMode = 2; showPicker = true }.buttonStyle(.bordered)
                Button("3.啟動EL2") { vm.bootRealKernel() }.buttonStyle(.borderedProminent)
            }.padding(.bottom)
        }
        .padding(.top)
        .fileImporter(isPresented: $showPicker, allowedContentTypes: [.data, .item]) { res in
            if case .success(let url) = res {
                if pickerMode == 1 { vm.loadKernel(url: url) } else { vm.attachDisk(url: url) }
            }
        }
    }
}
