// Lists the OpenXR runtime's instance extensions (which features the headset runtime offers, e.g. hand tracking).
#include <cstdio>
#include <vector>
#include <openxr/openxr.h>
int main() {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> p(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, p.data());
    for (auto& e : p) printf("%s v%u\n", e.extensionName, e.extensionVersion);
    return 0;
}
