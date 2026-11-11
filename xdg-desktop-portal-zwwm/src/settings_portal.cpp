#include "settings_portal.hpp"

#include "screenshot_portal.hpp"

#include <QDBusMetaType>

namespace {
constexpr auto kAppearance = "org.freedesktop.appearance";
constexpr auto kColorScheme = "color-scheme";
// The one setting zwwm answers: 1 asks applications for their dark style.
constexpr uint kPreferDark = 1;

// An empty list or entry matches everything; "org.freedesktop.*" matches by prefix.
bool wantsAppearance(const QStringList& namespaces) {
  const QString name = QString::fromLatin1(kAppearance);
  if (namespaces.isEmpty()) return true;
  for (const QString& filter : namespaces) {
    if (filter.isEmpty() || filter == name) return true;
    if (filter.endsWith(QLatin1String(".*")) && name.startsWith(filter.chopped(1))) return true;
  }
  return false;
}
}

SettingsAdaptor::SettingsAdaptor(ScreenshotPortal* host) : QDBusAbstractAdaptor(host) {
  qDBusRegisterMetaType<QMap<QString, QVariantMap>>();
}

QMap<QString, QVariantMap> SettingsAdaptor::ReadAll(const QStringList& namespaces) {
  QMap<QString, QVariantMap> settings;
  if (wantsAppearance(namespaces))
    settings.insert(QString::fromLatin1(kAppearance),
                    {{QString::fromLatin1(kColorScheme), QVariant::fromValue(kPreferDark)}});
  return settings;
}

QDBusVariant SettingsAdaptor::Read(const QString& nameSpace, const QString& key) {
  if (nameSpace == QLatin1String(kAppearance) && key == QLatin1String(kColorScheme))
    return QDBusVariant(QVariant::fromValue(kPreferDark));
  static_cast<ScreenshotPortal*>(parent())->sendErrorReply(
      QStringLiteral("org.freedesktop.portal.Error.NotFound"), QStringLiteral("Requested setting not found"));
  return {};
}
