#include "httpserver.h"
#include <QDebug>
#include <QJsonArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QFile>
#include <QDir>
#include <QByteArray>

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
        QStringList fields = str.split(","); // Assuming fields are comma-separate
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

QList<QByteArray> customSplit(const QByteArray& input, const QByteArray& delimiter) {
    QList<QByteArray> result;
    int startIndex = 0;

    while (true) {
        int delimiterIndex = input.indexOf(delimiter, startIndex);
        if (delimiterIndex == -1) {
            result.append(input.mid(startIndex)); // Add the remaining part
            break;
        }
        result.append(input.mid(startIndex, delimiterIndex - startIndex));
        startIndex = delimiterIndex + delimiter.size();
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

    if (method == "GET" && path.startsWith("/records")) {
        // http://127.0.0.1:8080/records?startDate=2024-11-13&endDate=2024-11-23&pageSize=10&page=1
        QUrl url(path);
        QUrlQuery queryParams(url.query());

        // Query Parameters
        QString startDateStr = queryParams.queryItemValue("startDate");
        QString endDateStr = queryParams.queryItemValue("endDate");
        QString plateNumber = queryParams.queryItemValue("plateNumber");
        QString entryGateStr = queryParams.queryItemValue("entryGate");
        QString exitGateStr = queryParams.queryItemValue("exitGate");
        int pageSize = queryParams.queryItemValue("pageSize").toInt();
        int page = queryParams.queryItemValue("page").toInt();

        // Validate Dates
        QDate startDate = QDate::fromString(startDateStr, "yyyy-MM-dd");
        QDate endDate = QDate::fromString(endDateStr, "yyyy-MM-dd");

        if (!startDate.isValid() || !endDate.isValid()) {
            sendResponse(socket, "Invalid date format. Use yyyy-MM-dd.", 400);
            return;
        }

        // Parse entryGate and exitGate as lists of integers
        QStringList entryGateList = entryGateStr.split(",", Qt::SkipEmptyParts);
        QList<int> entryGates;
        for (const QString& gate : entryGateList) {
            bool ok;
            int gateNumber = gate.toInt(&ok);
            if (ok) {
                entryGates.append(gateNumber);
            }
        }

        QStringList exitGateList = exitGateStr.split(",", Qt::SkipEmptyParts);
        QList<int> exitGates;
        for (const QString& gate : exitGateList) {
            bool ok;
            int gateNumber = gate.toInt(&ok);
            if (ok) {
                exitGates.append(gateNumber);
            }
        }

        // Fetch records using the updated DatabaseResult structure
        DatabaseResult result = DatabaseManager::instance().getRecordsByFilters(
            startDate, endDate, plateNumber, entryGates, exitGates, pageSize, page
            );

        // JSON 변환 및 응답 생성
        QJsonArray jsonArray;
        for (const auto& record : result.records) {
            QJsonObject jsonObject;
            for (const auto& key : record.keys()) {
                jsonObject[key] = QJsonValue::fromVariant(record[key]);
            }
            jsonArray.append(jsonObject);
        }

        // 전체 레코드 수와 데이터를 포함한 응답 생성
        QJsonObject response;
        response["data"] = jsonArray;
        response["totalRecords"] = result.totalRecords;

        QJsonDocument jsonDoc(response);
        sendResponse(socket, jsonDoc.toJson(), 200);
    } else if (method == "GET" && path.startsWith("/images/")){
        // 요청된 경로를 실제 파일 시스템 경로로 변환
        QString filePath = QString("C:/Users/3kati/Desktop/db_qt/images") + path.mid(7); // "/images/" 이후 경로 추가
        QFile file(filePath);

        // 파일 존재 여부 확인
        if (!file.exists()) {
            sendResponse(socket, "Image not found", 404);
            return;
        }

        // 파일 열기
        if (!file.open(QIODevice::ReadOnly)) {
            sendResponse(socket, "Failed to open image", 500);
            return;
        }

        QByteArray imageData = file.readAll();
        file.close();

        // 이미지 데이터를 반환
        QByteArray httpResponse = QString("HTTP/1.1 200 OK\r\n"
                                          "Content-Type: image/jpeg\r\n"
                                          "Content-Length: %1\r\n\r\n")
                                      .arg(imageData.size())
                                      .toUtf8();
        httpResponse.append(imageData);
        socket->write(httpResponse);
        socket->flush();
        socket->disconnectFromHost();
        return;
    } else if (method == "GET" && path.startsWith("/gatefees")) {
        QSqlQuery query("SELECT GateNumber, GateFee FROM GATELIST");

        QJsonObject response;
        while (query.next()) {
            int gateNumber = query.value("GateNumber").toInt();
            int gateFee = query.value("GateFee").toInt();
            response[QString::number(gateNumber)] = gateFee;
        }

        QJsonDocument jsonDoc(response);
        sendResponse(socket, jsonDoc.toJson(), 200);
    }else if (method == "POST" && path == "/records") {
        QString body = request.split("\r\n\r\n").last();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(body.toUtf8());
        QJsonObject jsonObject = jsonDoc.object();

        QString entryTime = jsonObject.value("EntryTime").toString();
        QString plateNumber = jsonObject.value("PlateNumber").toString();
        int gateNumber = jsonObject.value("GateNumber").toInt();

        //add record in HIGHPASS_RECORD table.
        int newRecordID = DatabaseManager::instance().addHighPassRecord(entryTime, plateNumber, gateNumber);
        if (newRecordID != -1) {
            sendResponse(socket, "Record added successfully", 200);

            //insert data in BILL table.
            if(dbManager.checkIsEnterGate(gateNumber)){
                dbManager.insertEnterStepBill(plateNumber, gateNumber, newRecordID);
            }else {
                dbManager.insertExitStepBill(plateNumber, gateNumber, newRecordID);
            }

        } else {
            sendResponse(socket, "Failed to add record", 500);
        }
    } else if (method == "POST" && path == "/upload") {
        // Content-Type 헤더에서 boundary 추출
        QString contentType = request.section("Content-Type: ", 1).section("\r\n", 0, 0).trimmed();
        if (!contentType.startsWith("multipart/form-data;")) {
            sendResponse(socket, "Invalid Content-Type", 400);
            return;
        }

        QString boundary = "--" + contentType.section("boundary=", 1).trimmed();
        if (boundary.isEmpty()) {
            sendResponse(socket, "Boundary not found", 400);
            return;
        }

        qDebug() << "===== requestData ===== \n"<<requestData;
        // 본문 데이터 추출
        QByteArray bodyData = customSplit(requestData, "\r\n\r\n").last();
        //qDebug() << "===== body ===== \n"<<bodyData;

        QList<QByteArray> parts = customSplit(bodyData, boundary.toUtf8());
        //for( const QByteArray& part : parts){
        //    qDebug() << "===== part ===== \n"<< part;
        //}

        bool fileSaved = false;
        for (const QByteArray& part : parts) {
            if (part.contains("Content-Disposition: form-data;") && part.contains("filename=")) {
                // 파일 데이터 추출
                int dataIndex = part.indexOf("\r\n\r\n") + 4;
                QByteArray fileData = part.mid(dataIndex).trimmed();

                // 파일 이름 추출
                QString contentDisposition = QString::fromUtf8(part);
                QString fileName = contentDisposition.section("filename=", 1).section("\"", 1, 1);
                if (fileName.isEmpty()) fileName = "uploaded_file"; // 기본 이름 설정

                // 파일 저장
                QString filePath = QDir::currentPath() + "/uploads/" + fileName;
                QDir dir("uploads");
                if (!dir.exists()) dir.mkpath(".");

                QFile file(filePath);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(fileData);
                    file.close();
                    fileSaved = true;
                    sendResponse(socket, "File uploaded successfully", 200);
                } else {
                    sendResponse(socket, "Failed to save file", 500);
                }
            }
        }

        if (!fileSaved) {
            sendResponse(socket, "No file found in the request", 400);
        }
    }  else if (method == "POST" && path == "/camera") {
        // 요청 본문에서 JSON 데이터 추출
        qDebug() << "Received 원본:" << request;
        QString body = request.mid(request.indexOf("\r\n\r\n") + 4).trimmed(); // JSON 본문 추출
        qDebug() << "Received body:" << body;

        // JSON 파싱
        QJsonDocument jsonDoc = QJsonDocument::fromJson(body.toUtf8());
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            sendResponse(socket, "Invalid JSON format", 400);
            return;
        }

        QJsonObject jsonObject = jsonDoc.object();
        QString cameraName = jsonObject.value("Camera_Name").toString();
        QString rtspUrl = jsonObject.value("Camera_RTSP_URL").toString();

        if (cameraName.isEmpty() || rtspUrl.isEmpty()) {
            sendResponse(socket, "Missing Camera_Name or Camera_RTSP_URL", 400);
            return;
        }
        qDebug() << "Camera Name:" << cameraName;
        qDebug() << "RTSP URL:" << rtspUrl;

        // 데이터베이스에 카메라 추가
        int newCameraID = DatabaseManager::instance().addCamera(cameraName, rtspUrl);
        if (newCameraID != -1) {
            // 성공 응답 생성
            QJsonObject responseObj;
            responseObj["message"] = "Camera added successfully";
            responseObj["camera_ID"] = newCameraID;

            QJsonDocument responseDoc(responseObj);
            sendResponse(socket, responseDoc.toJson(), 200);
        } else {
            sendResponse(socket, "Failed to add camera to the database", 500);
        }
    } else if (method == "GET" && path == "/cameras") {
        // 모든 카메라 리스트 가져오기
        QList<QVariantMap> cameras = DatabaseManager::instance().getAllCameras(); // 카메라 데이터 가져오기
        QJsonArray jsonArray;

        for (const auto& camera : cameras) {
            QJsonObject jsonObject;
            jsonObject["Camera_ID"] = camera["camera_ID"].toInt();
            jsonObject["Camera_Name"] = camera["Camera_Name"].toString();
            jsonObject["Camera_RTSP_URL"] = camera["Camera_RTSP_URL"].toString();
            jsonArray.append(jsonObject);
        }

        QJsonDocument jsonDoc(jsonArray);
        sendResponse(socket, jsonDoc.toJson(), 200);
    } else {
        sendResponse(socket, "Not Found", 404);
    }
}

void HttpServer::sendResponse(QTcpSocket* socket, const QByteArray& body, int statusCode) {
    // Prepare headers
    QByteArray responseHeaders;

    // Basic HTTP status line (e.g., "HTTP/1.1 200 OK")
    responseHeaders.append("HTTP/1.1 " + QByteArray::number(statusCode) + " OK\r\n");

    // Content-Type header (for JSON response)
    responseHeaders.append("Content-Type: application/json\r\n");

    // Content-Length header
    responseHeaders.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n");

    // End of headers (empty line)
    responseHeaders.append("\r\n");

    // Send headers and body
    socket->write(responseHeaders);
    socket->write(body);
    socket->flush();
}
