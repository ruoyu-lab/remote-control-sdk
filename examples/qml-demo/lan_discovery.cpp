#include "lan_discovery.hpp"

#include <rcsdk/rcsdk.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QUuid>

#include <algorithm>

namespace {

constexpr auto appTag = "rcsdk-demo";

QString platformName() {
#if defined(Q_OS_IOS)
    return QStringLiteral("iOS");
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("Android");
#elif defined(__APPLE__)
    return QStringLiteral("macOS");
#elif defined(_WIN32)
    return QStringLiteral("Windows");
#else
    return QStringLiteral("other");
#endif
}

} // namespace

// ---- DeviceList -----------------------------------------------------------------

QVariant DeviceList::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(devices_.size())) {
        return {};
    }
    const Device& d = devices_[size_t(index.row())];
    switch (role) {
    case IdRole:
        return d.id;
    case NameRole:
        return d.name;
    case PlatformRole:
        return d.platform;
    case AddressRole:
        return d.address.toString();
    case PortRole:
        return d.port;
    case AcceptingRole:
        return d.accepting;
    case VersionRole:
        return d.version;
    case TcpRole:
        return d.tcp;
    default:
        return {};
    }
}

QHash<int, QByteArray> DeviceList::roleNames() const {
    return {{IdRole, "deviceId"},     {NameRole, "name"},           {PlatformRole, "platform"},
            {AddressRole, "address"}, {PortRole, "port"},           {AcceptingRole, "accepting"},
            {VersionRole, "version"},     {TcpRole, "tcp"}};
}

void DeviceList::update(const Device& device) {
    const auto it =
        std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.id == device.id; });
    if (it == devices_.end()) {
        beginInsertRows({}, int(devices_.size()), int(devices_.size()));
        devices_.push_back(device);
        endInsertRows();
        emit countChanged();
        return;
    }
    *it = device;
    const QModelIndex changed = index(int(it - devices_.begin()));
    emit dataChanged(changed, changed);
}

void DeviceList::expire(qint64 now, qint64 maxAge) {
    for (int i = int(devices_.size()) - 1; i >= 0; --i) {
        if (now - devices_[size_t(i)].seen > maxAge) {
            beginRemoveRows({}, i, i);
            devices_.erase(devices_.begin() + i);
            endRemoveRows();
            emit countChanged();
        }
    }
}

const DeviceList::Device* DeviceList::find(const QString& id) const {
    const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.id == id; });
    return it == devices_.end() ? nullptr : &*it;
}

// ---- LanDiscovery ---------------------------------------------------------------

LanDiscovery::LanDiscovery(QObject* parent)
    : QObject(parent), id_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    clock_.start();
    // Several instances on one machine (testing) share the port.
    socket_.bind(QHostAddress::AnyIPv4, discoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    connect(&socket_, &QUdpSocket::readyRead, this, &LanDiscovery::receive);
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        announce();
        devices_.expire(clock_.elapsed(), 4000);
    });
    timer_.start();
}

void LanDiscovery::setAnnouncement(const QString& name, int port, bool accepting, bool tcp) {
    const bool changed = name != name_ || port != port_ || accepting != accepting_ || tcp != tcp_;
    name_ = name;
    port_ = port;
    accepting_ = accepting;
    tcp_ = tcp;
    if (changed) {
        announce(); // Others see the change at once.
    }
}

QStringList LanDiscovery::localAddresses() {
    QStringList result;
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                result << entry.ip().toString();
            }
        }
    }
    return result;
}

void LanDiscovery::announce() {
    QJsonObject message{{"app", appTag},         {"id", id_},          {"name", name_},
                        {"platform", platformName()}, {"port", port_}, {"accepting", accepting_},
                        {"version", QString::fromUtf8(rcs_version())},
                        {"transport", tcp_ ? "tcp" : "udp"}};
    const QByteArray datagram = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QList<QHostAddress> targets{QHostAddress::Broadcast};
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::CanBroadcast) ||
            (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull()) {
                targets << entry.broadcast();
            }
        }
    }
    for (const QHostAddress& target : targets) {
        socket_.writeDatagram(datagram, target, discoveryPort);
    }
}

void LanDiscovery::receive() {
    while (socket_.hasPendingDatagrams()) {
        QByteArray datagram(int(socket_.pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        socket_.readDatagram(datagram.data(), datagram.size(), &sender);
        const QJsonObject message = QJsonDocument::fromJson(datagram).object();
        if (message.value("app").toString() != QLatin1String(appTag) || message.value("id").toString() == id_) {
            continue; // Not ours, or our own announcement.
        }
        DeviceList::Device device;
        device.id = message.value("id").toString();
        device.name = message.value("name").toString();
        device.platform = message.value("platform").toString();
        device.version = message.value("version").toString();
        device.port = message.value("port").toInt();
        device.accepting = message.value("accepting").toBool();
        device.tcp = message.value("transport").toString() == QLatin1String("tcp");
        device.address = QHostAddress(sender.toIPv4Address()); // Plain IPv4, not ::ffff:a.b.c.d.
        device.seen = clock_.elapsed();
        if (!device.id.isEmpty() && device.port > 0) {
            devices_.update(device);
        }
    }
}
