// The embedded ReXGlue runtime must not attach a graphics debugger. ReXGlue's
// optional RenderDoc probe sees this library, finds no RENDERDOC_GetAPI export,
// and cleanly continues without capture support.
extern "C" int whitty_renderdoc_disabled = 1;
