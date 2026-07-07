#include "core/qrcode_print.hpp"

#if defined(KIKO_HAVE_QRCODE)
#include <qrcodegen.hpp>
#endif

#include <sstream>

namespace kiko {

namespace {

#if defined(KIKO_HAVE_QRCODE)
std::string qr_to_svg(const qrcodegen::QrCode& qr) {
  constexpr int border = 2;
  const int size = qr.getSize();
  const int view_size = size + border * 2;
  std::ostringstream out;
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << view_size << ' ' << view_size
      << "\" shape-rendering=\"crispEdges\">";
  out << "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>";
  out << "<path fill=\"#000\" d=\"";
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (!qr.getModule(x, y)) continue;
      out << 'M' << (x + border) << ',' << (y + border) << "h1v1h-1z";
    }
  }
  out << "\"/></svg>";
  return out.str();
}
#endif

}  // namespace

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

std::optional<std::string> qrcode_svg(const std::string& text) {
#if defined(KIKO_HAVE_QRCODE)
  try {
    return qr_to_svg(qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW));
  } catch (...) {
    return std::nullopt;
  }
#else
  (void)text;
  return std::nullopt;
#endif
}

}  // namespace kiko
