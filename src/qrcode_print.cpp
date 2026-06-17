#include "qrcode_print.hpp"

#if defined(KIKO_HAVE_QRCODE)
#include <qrcodegen.hpp>
#endif

namespace kiko {

void print_qrcode(std::ostream& out, const std::string& text) {
#if defined(KIKO_HAVE_QRCODE)
  const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
  const int border = 1;
  const int size = qr.getSize();
  for (int y = -border; y < size + border; ++y) {
    for (int x = -border; x < size + border; ++x) {
      out << (qr.getModule(x, y) ? "██" : "  ");
    }
    out << '\n';
  }
#else
  (void)text;
  (void)out;
#endif
}

}  // namespace kiko
