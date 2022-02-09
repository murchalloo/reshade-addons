# ReshadeAddons
Addons for reshade 5.0

## 99-raw_depth_buffer_export
Reshade addon to export 32 bit .exr Depth texture, created from Depth Buffer. Also displaying current Depth texture and info (resolution, format of texture and buffer) in addon overlay. [DepthToAddon.fx](https://github.com/murchalloo/murchFX/blob/main/Shaders/DepthToAddon.fx) shader is required and should be on. Capture key is F10, not changable at this moment, but it captures Color image as well in .bmp, so you get Color and Depth of the current frame. Images saving to .exe root folder with **BackBuffer** postfix for color and **DepthBuffer** for depth.
