#include "httpserver.h"
#include <QDebug>
#include <QJsonArray>
#include <QFile>
#include "PlateRecordInterface.h"
#include <QString>
#include <QStringList>
#include <QTcpServer>

HttpServer::HttpServer(DatabaseManager& dbManager, QObject* parent)
    : QTcpServer(parent), dbManager(dbManager) {}

void HttpServer::startServer(quint16 port) {
    if (!this->listen(QHostAddress::Any, port)) {
        qCritical() << "Failed to start server on port" << port;
    } else {
        qDebug() << "Server started on port" << port;
    }
}

void HttpServer::incomingConnection(qintptr socketDescriptor) {
    QTcpSocket* socket = new QTcpSocket();
    if (!socket->setSocketDescriptor(socketDescriptor)) {
        qDebug() << "Failed to set socket descriptor";
        socket->deleteLater();
        return;
    }

    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        handleRequest(socket);
    });
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
}

QList<QVariantMap> convertToQVariantMapList(const QStringList& stringList) {
    QList<QVariantMap> result;

    for (const QString& str : stringList) {
        QStringList fields = str.split(","); // Assuming fields are comma-separated
        if (fields.size() < 4) {
            qWarning() << "Skipping malformed entry:" << str;
            continue;
        }

        QVariantMap map;
        map["ID"] = fields[0].trimmed();
        map["EntryTime"] = fields[1].trimmed();
        map["PlateNumber"] = fields[2].trimmed();
        map["GateNumber"] = fields[3].trimmed();
        result.append(map);
    }

    return result;
}

void HttpServer::handleRequest(QTcpSocket* socket) {
    QByteArray requestData = socket->readAll();
    QString request = QString::fromUtf8(requestData);

    // Parse HTTP Request (simple parser for GET and POST)
    QString method, path;
    QTextStream stream(&request);
    stream >> method >> path;

    if (method == "GET" && path == "/records") {
        PlateRecordInterface plateRecord(DatabaseManager::instance());
        QStringList strListRecords = plateRecord.getAllRecords(); // 데이터 가져오기
        QList<QVariantMap> records = convertToQVariantMapList(strListRecords);
        QJsonArray jsonArray;

        for (const auto& record : records) {
            QJsonObject jsonObject;
            jsonObject["ID"] = record["ID"].toInt();
            jsonObject["EntryTime"] = record["EntryTime"].toString();
            jsonObject["PlateNumber"] = record["PlateNumber"].toString();
            jsonObject["GateNumber"] = record["GateNumber"].toInt();
            jsonArray.append(jsonObject);
        }

        QJsonDocument jsonDoc(jsonArray);
        sendResponse(socket, jsonDoc.toJson(), 200);
    } else if (method == "POST" && path == "/records") {
        QString body = request.split("\r\n\r\n").last();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(body.toUtf8());
        QJsonObject jsonObject = jsonDoc.object();

        QString entryTime = jsonObject.value("EntryTime").toString();
        QString plateNumber = jsonObject.value("PlateNumber").toString();
        int gateNumber = jsonObject.value("GateNumber").toInt();

        if (dbManager.insertHighPassRecord(entryTime, plateNumber, gateNumber)) {
            sendResponse(socket, "Record added successfully", 200);
        } else {
            sendResponse(socket, "Failed to add record", 500);
        }
    } else {
        sendResponse(socket, "Not Found", 404);
    }
}

void HttpServer::sendResponse(QTcpSocket* socket, const QString& response, int statusCode) {
    QString statusMessage = (statusCode == 200) ? "OK" :
                            (statusCode == 404) ? "Not Found" : "Internal Server Error";

    QByteArray httpResponse = QString("HTTP/1.1 %1 %2\r\n"
                                      "Content-Type: application/json\r\n"
                                      "Content-Length: %3\r\n"
                                      "\r\n%4")
                                  .arg(statusCode)
                                  .arg(statusMessage)
                                  .arg(response.size())
                                  .arg(response)
                                  .toUtf8();
    socket->write(httpResponse);
    socket->flush();
    socket->disconnectFromHost();
}