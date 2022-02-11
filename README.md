# reshade-addons
Addons for reshade 5.0

## 99-frame_capture
Reshade addon to export 32 bit .exr depth and normal textures, created from Depth Buffer. Also displaying current depth and normal textures and info (name, resolution, format of textures) in addon overlay. Last version of [DepthToAddon.fx](https://github.com/murchalloo/murchFX/blob/main/Shaders/DepthToAddon.fx) shader is required and should it be on. Capture key is F10, not changable at this moment, but it captures Color image as well in .bmp. Images saving to .exe root folder with **BackBuffer** postfix for color, **DepthBuffer** for depth and **NormalMap** for normal.
