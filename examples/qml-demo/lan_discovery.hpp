// Finding other demo instances on the LAN, for the demo only: the SDK leaves
// discovery to the host. Each instance broadcasts a small JSON datagram every
// second on UDP port 47901, to every IPv4 interface's broadcast address, and
// lists the instances it hears; one silent for 4 seconds leaves the list.
// An announcement carries the transport the host listens on: both ends of a
// session must use the same one.
#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>
#include <QtQml/qqmlregistration.h>

#include <vector>

class DeviceList : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by LanDiscovery")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PlatformRole,
        AddressRole,
        PortRole,
        AcceptingRole,
        VersionRole,
        TcpRole
    };

    struct Device {
        QString id, name, platform, version;
        QHostAddress address;
        int port = 0;
        bool accepting = false, tcp = false;
        qint64 seen = 0; // Milliseconds of LanDiscovery's clock.
    };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(devices_.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(devices_.size()); }

    void update(const Device& device);
    void expire(qint64 now, qint64 maxAge);
    const Device* find(const QString& id) const;

signals:
    void countChanged();

private:
    std::vector<Device> devices_;
};

class LanDiscovery : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 discoveryPort = 47901;

    explicit LanDiscovery(QObject* parent = nullptr);

    DeviceList* devices() { return &devices_; }
    QString id() const { return id_; }
    // What this instance announces.
    void setAnnouncement(const QString& name, int port, bool accepting, bool tcp);
    // This machine's IPv4 addresses, for display.
    static QStringList localAddresses();

private:
    void announce();
    void receive();

    QUdpSocket socket_;
    QTimer timer_;
    QElapsedTimer clock_;
    DeviceList devices_;
    QString id_, name_;
    int port_ = 0;
    bool accepting_ = false, tcp_ = false;
};
