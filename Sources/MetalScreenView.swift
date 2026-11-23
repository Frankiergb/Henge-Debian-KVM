import SwiftUI
import MetalKit

struct MetalScreenView: UIViewRepresentable {
    func makeCoordinator() -> Renderer { Renderer() }
    func makeUIView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.framebufferOnly = false
        view.preferredFramesPerSecond = 60
        view.delegate = context.coordinator
        return view
    }
    func updateUIView(_ uiView: MTKView, context: Context) {}

    class Renderer: NSObject, MTKViewDelegate {
        private var tex: MTLTexture?
        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
        func draw(in view: MTKView) {
            var w: Int32 = 0, h: Int32 = 0
            guard let ptr = virtio_get_framebuffer(&w, &h), w > 0, h > 0,
                  let dev = view.device, let drawable = view.currentDrawable,
                  let cmd = dev.makeCommandQueue()?.makeCommandBuffer(),
                  let blit = cmd.makeBlitCommandEncoder() else { return }
            if tex == nil {
                let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: Int(w), height: Int(h), mipmapped: false)
                tex = dev.makeTexture(descriptor: desc)
            }
            guard let t = tex else { return }
            t.replace(region: MTLRegionMake2D(0, 0, Int(w), Int(h)), mipmapLevel: 0, withBytes: ptr, bytesPerRow: Int(w) * 4)
            let cw = min(t.width, drawable.texture.width), ch = min(t.height, drawable.texture.height)
            blit.copy(from: t, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: MTLSize(width: cw, height: ch, depth: 1), to: drawable.texture, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding(); cmd.present(drawable); cmd.commit()
        }
    }
}
