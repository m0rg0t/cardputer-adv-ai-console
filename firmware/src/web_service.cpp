#include "recorder/web_service.h"

#include <FS.h>
#include <WiFi.h>

namespace cardputer_recorder {
namespace {

constexpr const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cardputer Recorder</title><style>
:root{color-scheme:dark;--bg:#0b1020;--card:#151c31;--line:#29334e;--accent:#6be6c1;--muted:#9ba8c7;--danger:#ff7885}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:#f5f7ff;font:15px system-ui,sans-serif}main{width:min(920px,calc(100% - 28px));margin:28px auto}h1{font-size:26px;margin:0}h2{font-size:17px;margin:0 0 14px}.lead{color:var(--muted);margin:6px 0 22px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:17px;margin-bottom:14px}.metric{font-size:21px;color:var(--accent);margin-top:4px}.muted,small{color:var(--muted)}table{width:100%;border-collapse:collapse}th,td{text-align:left;border-top:1px solid var(--line);padding:11px 7px}th{color:var(--muted);font-weight:500}a{color:var(--accent)}button{border:0;border-radius:9px;background:var(--accent);color:#061711;padding:10px 16px;font-weight:700;cursor:pointer}button:disabled{cursor:wait;opacity:.55}.file-actions{display:flex;align-items:center;justify-content:flex-end;gap:5px;white-space:nowrap}.file-action{width:34px;height:34px;padding:0;border:1px solid var(--line);background:#0d1427;color:var(--accent);font-size:17px}.file-action.danger{color:var(--danger)}label{display:grid;gap:6px;color:var(--muted)}input,select{width:100%;padding:9px;border:1px solid var(--line);border-radius:8px;background:#0d1427;color:#fff}.form{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:13px}.actions{display:flex;align-items:center;gap:12px;margin-top:15px}audio{width:180px;height:34px}@media(max-width:600px){audio{width:125px}.optional{display:none}th,td{padding:10px 4px}.file-actions{flex-wrap:wrap}}
</style></head><body><main><h1>Cardputer Recorder</h1><p class="lead" id="address">Локальная панель устройства</p>
<section class="grid"><div class="card"><div class="muted">Состояние</div><div class="metric" id="mode">…</div></div><div class="card"><div class="muted">Wi-Fi</div><div class="metric" id="wifi">…</div><small id="ip"></small></div><div class="card"><div class="muted">microSD</div><div class="metric" id="storage">…</div></div><div class="card"><div class="muted">Батарея</div><div class="metric" id="battery">…</div></div></section>
<section class="card"><h2>Аудиофайлы</h2><div id="recordings" class="muted">Загрузка…</div></section>
<section class="card"><h2>Настройки</h2><form id="settings"><div class="form"><label>Имя в сети (.local)<input name="web_hostname" maxlength="32" pattern="[a-zA-Z0-9-]+"></label><label>Яркость<select name="brightness_percent"><option>10</option><option>30</option><option>50</option><option>70</option><option>90</option><option>100</option></select></label><label>Качество записи<select name="compact_audio"><option value="false">16 kHz</option><option value="true">8 kHz compact</option></select></label><label>Сохранение при заряде<select name="low_battery_save_percent"><option value="0">Выкл.</option><option>1</option><option>5</option><option>10</option></select></label><label>Шаг перемотки<select name="seek_step_seconds"><option>5</option><option>10</option><option>20</option><option>60</option></select></label><label>Сортировка<select name="library_sort"><option value="0">Сначала новые</option><option value="1">Сначала старые</option><option value="2">По статусу</option><option value="3">По имени</option></select></label></div><div class="actions"><button>Сохранить</button><span id="saved" class="muted"></span></div></form></section>
<script>
const $=s=>document.querySelector(s), esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
async function json(url,options){const r=await fetch(url,options);const x=await r.json();if(!r.ok)throw Error(x.error||r.statusText);return x}
function bytes(n){for(const u of ['B','KB','MB','GB']){if(n<1024||u==='GB')return (n<10&&u!=='B'?n.toFixed(1):Math.round(n))+' '+u;n/=1024}}
async function status(){try{const x=await json('/api/status');$('#mode').textContent=x.mode;$('#wifi').textContent=x.wifi.connected?x.wifi.ssid:'Не подключён';$('#ip').textContent=x.wifi.ip||'';$('#storage').textContent=x.storage.mounted?bytes(x.storage.used)+' / '+bytes(x.storage.capacity):'Нет карты';$('#battery').textContent=x.battery.valid?x.battery.percent+'%':'—';$('#address').textContent=x.web.address}catch(e){$('#mode').textContent='Недоступен'}}
async function recordings(){try{const x=await json('/api/recordings');if(!x.items.length){$('#recordings').textContent='Записей пока нет';return}$('#recordings').innerHTML='<table><thead><tr><th>Имя</th><th class="optional">Размер</th><th>Прослушать</th><th aria-label="Действия"></th></tr></thead><tbody>'+x.items.map(f=>{const q=encodeURIComponent(f.name),u='/api/recordings/download?name='+q,n=esc(f.name);return `<tr><td>${n}<br><small>${esc(f.status)}</small></td><td class="optional">${bytes(f.size)}</td><td><audio controls preload="none" src="${u}"></audio></td><td><div class="file-actions"><a href="${u}" download title="Скачать ${n}" aria-label="Скачать ${n}">&#8681;</a><button class="file-action" data-action="rename" data-name="${n}" title="Переименовать ${n}" aria-label="Переименовать ${n}">&#9998;</button><button class="file-action danger" data-action="delete" data-name="${n}" title="Удалить ${n}" aria-label="Удалить ${n}">&#128465;</button></div></td></tr>`}).join('')+'</tbody></table>'}catch(e){$('#recordings').textContent=e.message}}
$('#recordings').addEventListener('click',async e=>{const b=e.target.closest('button[data-action]');if(!b)return;const name=b.dataset.name;if(b.dataset.action==='rename'){const base=name.replace(/\.wav$/i,''),next=prompt('Новое имя файла',base);if(next===null||next.trim()===base)return;b.disabled=true;try{await json('/api/recordings',{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,new_name:next})});await recordings()}catch(error){alert(error.message);b.disabled=false}}else{if(!confirm(`Удалить «${name}»? Это действие нельзя отменить.`))return;b.disabled=true;try{await json('/api/recordings',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({name})});await recordings()}catch(error){alert(error.message);b.disabled=false}}});
async function settings(){const x=await json('/api/settings'),f=$('#settings');for(const [k,v] of Object.entries(x))if(f.elements[k])f.elements[k].value=String(v)}
$('#settings').addEventListener('submit',async e=>{e.preventDefault();const data=Object.fromEntries(new FormData(e.target));$('#saved').textContent='Сохраняю…';try{const x=await json('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});$('#saved').textContent='Сохранено';if(x.address)$('#address').textContent=x.address;setTimeout(()=>location.hostname.endsWith('.local')&&x.address?location.href=x.address:location.reload(),700)}catch(e){$('#saved').textContent=e.message}});
status();recordings();settings().catch(e=>$('#saved').textContent=e.message);setInterval(status,5000);
</script></main></body></html>)HTML";

bool isWav(const String& name)
{
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".wav");
}

String recordingSidecarPath(const String& filename, const char* suffix)
{
    String path = "/" + filename;
    path.remove(path.length() - 4);
    path += suffix;
    return path;
}

constexpr std::uint16_t kMdnsPort = 5353;
const IPAddress kMdnsAddress(224, 0, 0, 251);

std::uint16_t readU16(const std::uint8_t* data)
{
    return (static_cast<std::uint16_t>(data[0]) << 8U) | data[1];
}

void appendU16(std::uint8_t* data, std::size_t& offset,
               std::uint16_t value)
{
    data[offset++] = static_cast<std::uint8_t>(value >> 8U);
    data[offset++] = static_cast<std::uint8_t>(value);
}

void appendU32(std::uint8_t* data, std::size_t& offset,
               std::uint32_t value)
{
    data[offset++] = static_cast<std::uint8_t>(value >> 24U);
    data[offset++] = static_cast<std::uint8_t>(value >> 16U);
    data[offset++] = static_cast<std::uint8_t>(value >> 8U);
    data[offset++] = static_cast<std::uint8_t>(value);
}

bool readDnsName(const std::uint8_t* packet, std::size_t length,
                 std::size_t& offset, String& name)
{
    name = "";
    while (offset < length) {
        const std::uint8_t labelLength = packet[offset++];
        if (labelLength == 0) {
            return true;
        }
        // Questions normally contain labels rather than compression pointers.
        // Rejecting compressed input keeps this responder small and bounded.
        if (labelLength > 63 || offset + labelLength > length) {
            return false;
        }
        if (name.length() > 0) {
            name += '.';
        }
        for (std::uint8_t index = 0; index < labelLength; ++index) {
            name += static_cast<char>(packet[offset++]);
        }
    }
    return false;
}

bool appendDnsName(std::uint8_t* packet, std::size_t capacity,
                   std::size_t& offset, const String& name)
{
    int start = 0;
    while (start < static_cast<int>(name.length())) {
        int end = name.indexOf('.', start);
        if (end < 0) {
            end = name.length();
        }
        const int labelLength = end - start;
        if (labelLength <= 0 || labelLength > 63 ||
            offset + labelLength + 1 >= capacity) {
            return false;
        }
        packet[offset++] = static_cast<std::uint8_t>(labelLength);
        for (int index = start; index < end; ++index) {
            packet[offset++] = static_cast<std::uint8_t>(name[index]);
        }
        start = end + 1;
    }
    packet[offset++] = 0;
    return true;
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

String urlDecode(const String& encoded)
{
    String decoded;
    decoded.reserve(encoded.length());
    for (std::size_t index = 0; index < encoded.length(); ++index) {
        if (encoded[index] == '%' && index + 2 < encoded.length()) {
            const int high = hexValue(encoded[index + 1]);
            const int low = hexValue(encoded[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        decoded += encoded[index] == '+' ? ' ' : encoded[index];
    }
    return decoded;
}

const char* statusText(int status)
{
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 409:
            return "Conflict";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
    }
}

}  // namespace

void WebService::begin(StorageService& storage, UploadService& uploader,
                       JsonWriter writeStatus, JsonWriter writeSettings,
                       SettingsApplier applySettings)
{
    storage_ = &storage;
    uploader_ = &uploader;
    writeStatus_ = std::move(writeStatus);
    writeSettings_ = std::move(writeSettings);
    applySettings_ = std::move(applySettings);
}

void WebService::configure(bool enabled, const String& hostname)
{
    String normalized = hostname;
    normalized.trim();
    normalized.toLowerCase();
    const bool changed = hostname_ != normalized;
    enabled_ = enabled;
    if (normalized.length() > 0) {
        hostname_ = normalized;
    }
    if (!enabled_) {
        stop();
    } else if (changed && mdnsStarted_) {
        mdnsUdp_.stop();
        mdnsStarted_ = false;
    }
}

void WebService::update(bool fileIoAllowed)
{
    fileIoAllowed_ = fileIoAllowed;
    if (!enabled_) {
        return;
    }
    startIfReady();
    if (serverStarted_ && WiFi.status() == WL_CONNECTED) {
        serviceMdns();
        WiFiClient client = server_.available();
        if (client) {
            handleClient(client);
            client.stop();
        }
    }
}

bool WebService::running() const
{
    return enabled_ && serverStarted_ && WiFi.status() == WL_CONNECTED;
}

String WebService::address() const
{
    return running() ? "http://" + hostname_ + ".local/" : String();
}

void WebService::startIfReady()
{
    if (WiFi.status() != WL_CONNECTED) {
        if (mdnsStarted_) {
            mdnsUdp_.stop();
            mdnsStarted_ = false;
        }
        return;
    }
    if (!serverStarted_) {
        server_.begin();
        server_.setNoDelay(true);
        serverStarted_ = true;
        Serial.println("[WEB] HTTP server started on port 80");
    }
    if (!mdnsStarted_) {
        mdnsStarted_ = mdnsUdp_.beginMulticast(kMdnsAddress, kMdnsPort) == 1;
        if (mdnsStarted_) {
            Serial.printf("[WEB] Open http://%s.local/\n",
                          hostname_.c_str());
        } else {
            Serial.println("[WEB] Could not start mDNS");
        }
    }
}

void WebService::serviceMdns()
{
    const int packetSize = mdnsUdp_.parsePacket();
    if (packetSize < 12 || packetSize > 512) {
        if (packetSize > 0) {
            while (mdnsUdp_.available()) {
                mdnsUdp_.read();
            }
        }
        return;
    }
    std::uint8_t query[512];
    const int received = mdnsUdp_.read(query, sizeof(query));
    if (received < 12 || (readU16(query + 2) & 0x8000U) != 0) {
        return;
    }
    const String expected = hostname_ + ".local";
    const std::uint16_t questionCount = readU16(query + 4);
    std::size_t offset = 12;
    bool requested = false;
    for (std::uint16_t question = 0;
         question < questionCount && offset < static_cast<std::size_t>(received);
         ++question) {
        String name;
        if (!readDnsName(query, received, offset, name) ||
            offset + 4 > static_cast<std::size_t>(received)) {
            return;
        }
        name.toLowerCase();
        const std::uint16_t type = readU16(query + offset);
        offset += 4;  // type and class
        if (name == expected && (type == 1 || type == 255)) {
            requested = true;
        }
    }
    if (!requested) {
        return;
    }

    std::uint8_t response[128] = {};
    offset = 0;
    appendU16(response, offset, 0);       // mDNS transaction ID
    appendU16(response, offset, 0x8400);  // response + authoritative
    appendU16(response, offset, 0);       // questions
    appendU16(response, offset, 1);       // answers
    appendU16(response, offset, 0);       // authority records
    appendU16(response, offset, 0);       // additional records
    if (!appendDnsName(response, sizeof(response), offset, expected)) {
        return;
    }
    appendU16(response, offset, 1);       // A
    appendU16(response, offset, 0x8001);  // IN + cache flush
    appendU32(response, offset, 120);     // TTL
    appendU16(response, offset, 4);
    const IPAddress ip = WiFi.localIP();
    for (std::uint8_t index = 0; index < 4; ++index) {
        response[offset++] = ip[index];
    }
    if (mdnsUdp_.beginMulticastPacket()) {
        mdnsUdp_.write(response, offset);
        mdnsUdp_.endPacket();
    }
}

void WebService::stop()
{
    if (mdnsStarted_) {
        mdnsUdp_.stop();
        mdnsStarted_ = false;
    }
    if (serverStarted_) {
        server_.stop();
        serverStarted_ = false;
    }
}

void WebService::handleClient(WiFiClient& client)
{
    client.setTimeout(1);
    String headers;
    headers.reserve(768);
    const unsigned long deadline = millis() + 750;
    while (client.connected() && millis() < deadline &&
           !headers.endsWith("\r\n\r\n")) {
        while (client.available() && !headers.endsWith("\r\n\r\n")) {
            headers += static_cast<char>(client.read());
            if (headers.length() > 2048) {
                sendError(client, 413, "HTTP headers too large");
                return;
            }
        }
        delay(1);
    }
    const int firstLineEnd = headers.indexOf("\r\n");
    if (firstLineEnd < 0 || !headers.endsWith("\r\n\r\n")) {
        sendError(client, 400, "Incomplete HTTP request");
        return;
    }
    const String requestLine = headers.substring(0, firstLineEnd);
    const int firstSpace = requestLine.indexOf(' ');
    const int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace <= 0 || secondSpace <= firstSpace) {
        sendError(client, 400, "Invalid HTTP request");
        return;
    }
    const String method = requestLine.substring(0, firstSpace);
    const String target = requestLine.substring(firstSpace + 1, secondSpace);

    std::size_t contentLength = 0;
    String lowerHeaders = headers;
    lowerHeaders.toLowerCase();
    const int lengthHeader = lowerHeaders.indexOf("\r\ncontent-length:");
    if (lengthHeader >= 0) {
        const int valueStart = lengthHeader + 17;
        const int valueEnd = lowerHeaders.indexOf("\r\n", valueStart);
        contentLength = headers.substring(valueStart, valueEnd).toInt();
    }
    if (contentLength > 2048) {
        sendError(client, 413, "Request body too large");
        return;
    }
    String body;
    body.reserve(contentLength);
    const unsigned long bodyDeadline = millis() + 750;
    while (body.length() < contentLength && client.connected() &&
           millis() < bodyDeadline) {
        while (client.available() && body.length() < contentLength) {
            body += static_cast<char>(client.read());
        }
        delay(1);
    }
    if (body.length() != contentLength) {
        sendError(client, 400, "Incomplete request body");
        return;
    }

    if (method == "GET" && target == "/") {
        const std::size_t length = strlen(kIndexHtml);
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; "
                      "charset=utf-8\r\nContent-Length: %u\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      static_cast<unsigned int>(length));
        client.write(reinterpret_cast<const std::uint8_t*>(kIndexHtml),
                     length);
    } else if (method == "GET" && target == "/api/status") {
        handleStatus(client);
    } else if (method == "GET" && target == "/api/settings") {
        handleSettingsGet(client);
    } else if (method == "POST" && target == "/api/settings") {
        handleSettingsPost(client, body);
    } else if (method == "GET" && target == "/api/recordings") {
        handleRecordings(client);
    } else if (method == "PATCH" && target == "/api/recordings") {
        handleRecordingRename(client, body);
    } else if (method == "DELETE" && target == "/api/recordings") {
        handleRecordingDelete(client, body);
    } else if (method == "GET" &&
               target.startsWith("/api/recordings/download?name=")) {
        handleRecordingDownload(
            client, urlDecode(target.substring(target.indexOf('=') + 1)));
    } else if (target == "/api/settings" || target == "/api/status" ||
               target == "/api/recordings") {
        sendError(client, 405, "Method not allowed");
    } else {
        sendError(client, 404, "Not found");
    }
}

void WebService::sendResponse(WiFiClient& client, int status,
                              const char* contentType, const String& body,
                              const String& extraHeaders)
{
    client.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                  "Content-Length: %u\r\nCache-Control: no-store\r\n",
                  status, statusText(status), contentType,
                  static_cast<unsigned int>(body.length()));
    if (extraHeaders.length() > 0) {
        client.print(extraHeaders);
    }
    client.print("Connection: close\r\n\r\n");
    client.print(body);
}

void WebService::sendJson(WiFiClient& client, JsonDocument& document,
                          int status)
{
    String body;
    serializeJson(document, body);
    sendResponse(client, status, "application/json; charset=utf-8", body);
}

void WebService::sendError(WiFiClient& client, int status,
                           const String& message)
{
    JsonDocument document;
    document["error"] = message;
    sendJson(client, document, status);
}

void WebService::handleStatus(WiFiClient& client)
{
    JsonDocument document;
    JsonObject root = document.to<JsonObject>();
    if (writeStatus_) {
        writeStatus_(root);
    }
    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["connected"] = uploader_ != nullptr && uploader_->wifiConnected();
    wifi["ssid"] = uploader_ != nullptr ? uploader_->wifiSsid() : "";
    wifi["ip"] = uploader_ != nullptr ? uploader_->localIp() : "";
    wifi["gateway"] = uploader_ != nullptr ? uploader_->shortStatus() : "";
    JsonObject storage = root["storage"].to<JsonObject>();
    storage["mounted"] = storage_ != nullptr && storage_->isMounted();
    storage["used"] = storage_ != nullptr ? storage_->usedBytes() : 0;
    storage["capacity"] =
        storage_ != nullptr ? storage_->capacityBytes() : 0;
    JsonObject web = root["web"].to<JsonObject>();
    web["hostname"] = hostname_;
    web["address"] = address();
    sendJson(client, document);
}

void WebService::handleSettingsGet(WiFiClient& client)
{
    JsonDocument document;
    JsonObject root = document.to<JsonObject>();
    if (writeSettings_) {
        writeSettings_(root);
    }
    sendJson(client, document);
}

void WebService::handleSettingsPost(WiFiClient& client, const String& body)
{
    if (!fileIoAllowed_) {
        sendError(client, 503,
                  "Устройство занято записью или воспроизведением");
        return;
    }
    JsonDocument request;
    const DeserializationError parsed = deserializeJson(request, body);
    if (parsed || !request.is<JsonObject>()) {
        sendError(client, 400, "Invalid JSON settings");
        return;
    }
    String error;
    if (!applySettings_ ||
        !applySettings_(request.as<JsonObjectConst>(), error)) {
        sendError(client, 400,
                  error.length() > 0 ? error : "Invalid settings");
        return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["address"] = "http://" + hostname_ + ".local/";
    sendJson(client, response);
}

void WebService::handleRecordings(WiFiClient& client)
{
    if (!fileIoAllowed_) {
        sendError(client, 503, "Аудиохранилище временно занято");
        return;
    }
    if (storage_ == nullptr || !storage_->isMounted()) {
        sendError(client, 503, "microSD is not mounted");
        return;
    }
    JsonDocument document;
    JsonArray items = document["items"].to<JsonArray>();
    constexpr std::size_t kMaxWebRecordings = 200;
    std::size_t recordingCount = 0;
    File directory = storage_->open("/", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        sendError(client, 500, "Could not read microSD");
        return;
    }
    File entry = directory.openNextFile();
    while (entry) {
        String name = entry.name();
        if (name.startsWith("/")) {
            name.remove(0, 1);
        }
        if (!entry.isDirectory() && isWav(name)) {
            ++recordingCount;
            if (items.size() < kMaxWebRecordings) {
                JsonObject item = items.add<JsonObject>();
                item["name"] = name;
                item["size"] = static_cast<std::uint32_t>(entry.size());
                item["modified"] =
                    static_cast<std::uint32_t>(entry.getLastWrite());
                item["status"] =
                    uploader_ != nullptr
                        ? uploader_->recordingStatus(
                              name,
                              static_cast<std::uint32_t>(entry.size()))
                        : "LOCAL";
            }
        }
        entry.close();
        entry = directory.openNextFile();
    }
    directory.close();
    document["total"] = recordingCount;
    document["truncated"] = recordingCount > kMaxWebRecordings;
    sendJson(client, document);
}

void WebService::handleRecordingDownload(WiFiClient& client,
                                         const String& name)
{
    if (!fileIoAllowed_) {
        sendError(client, 503, "Аудиохранилище временно занято");
        return;
    }
    if (storage_ == nullptr || !safeRecordingName(name)) {
        sendError(client, 400, "Invalid recording name");
        return;
    }
    const String path = "/" + name;
    File file = storage_->open(path.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        sendError(client, 404, "Recording not found");
        return;
    }
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
                  "Content-Length: %u\r\nContent-Disposition: inline; "
                  "filename=\"%s\"\r\nCache-Control: private, no-store\r\n"
                  "Connection: close\r\n\r\n",
                  static_cast<unsigned int>(file.size()), name.c_str());
    std::uint8_t buffer[2048];
    while (file.available() && client.connected()) {
        const std::size_t read = file.read(buffer, sizeof(buffer));
        if (read == 0 || client.write(buffer, read) != read) {
            break;
        }
        delay(0);
    }
    file.close();
}

void WebService::handleRecordingRename(WiFiClient& client,
                                       const String& body)
{
    if (!fileIoAllowed_) {
        sendError(client, 503, "Аудиохранилище временно занято");
        return;
    }
    if (storage_ == nullptr || !storage_->isMounted()) {
        sendError(client, 503, "microSD is not mounted");
        return;
    }
    JsonDocument request;
    if (deserializeJson(request, body) || !request.is<JsonObject>()) {
        sendError(client, 400, "Invalid JSON request");
        return;
    }
    const String oldName = request["name"] | "";
    String newName;
    if (!safeRecordingName(oldName) ||
        !normalizeRecordingName(request["new_name"] | "", newName)) {
        sendError(client, 400,
                  "Имя: до 32 символов A-Z, 0-9, пробел, - или _");
        return;
    }
    const String oldPath = "/" + oldName;
    const String newPath = "/" + newName;
    if (!storage_->exists(oldPath.c_str())) {
        sendError(client, 404, "Recording not found");
        return;
    }
    if (oldName == newName) {
        JsonDocument response;
        response["ok"] = true;
        response["name"] = newName;
        sendJson(client, response);
        return;
    }
    if (storage_->exists(newPath.c_str())) {
        sendError(client, 409, "Файл с таким именем уже существует");
        return;
    }
    const String oldRoute = recordingSidecarPath(oldName, ".ROUTE");
    const String newRoute = recordingSidecarPath(newName, ".ROUTE");
    const String oldMetadata =
        recordingSidecarPath(oldName, ".AGENT.JSON");
    const String newMetadata =
        recordingSidecarPath(newName, ".AGENT.JSON");
    const bool hasRoute = storage_->exists(oldRoute.c_str());
    const bool hasMetadata = storage_->exists(oldMetadata.c_str());
    if ((hasRoute && storage_->exists(newRoute.c_str())) ||
        (hasMetadata && storage_->exists(newMetadata.c_str()))) {
        sendError(client, 409,
                  "Служебные данные для нового имени уже существуют");
        return;
    }
    if (!storage_->rename(oldPath.c_str(), newPath.c_str())) {
        sendError(client, 500, "Could not rename recording");
        return;
    }
    if (hasRoute &&
        !storage_->rename(oldRoute.c_str(), newRoute.c_str())) {
        storage_->rename(newPath.c_str(), oldPath.c_str());
        sendError(client, 500, "Could not rename recording route");
        return;
    }
    if (hasMetadata &&
        !storage_->rename(oldMetadata.c_str(), newMetadata.c_str())) {
        if (hasRoute) {
            storage_->rename(newRoute.c_str(), oldRoute.c_str());
        }
        storage_->rename(newPath.c_str(), oldPath.c_str());
        sendError(client, 500, "Could not rename recording metadata");
        return;
    }
    JsonDocument response;
    response["ok"] = true;
    response["name"] = newName;
    sendJson(client, response);
}

void WebService::handleRecordingDelete(WiFiClient& client,
                                       const String& body)
{
    if (!fileIoAllowed_) {
        sendError(client, 503, "Аудиохранилище временно занято");
        return;
    }
    if (storage_ == nullptr || !storage_->isMounted()) {
        sendError(client, 503, "microSD is not mounted");
        return;
    }
    JsonDocument request;
    if (deserializeJson(request, body) || !request.is<JsonObject>()) {
        sendError(client, 400, "Invalid JSON request");
        return;
    }
    const String name = request["name"] | "";
    if (!safeRecordingName(name)) {
        sendError(client, 400, "Invalid recording name");
        return;
    }
    const String path = "/" + name;
    if (!storage_->exists(path.c_str())) {
        sendError(client, 404, "Recording not found");
        return;
    }
    if (!storage_->remove(path.c_str())) {
        sendError(client, 500, "Could not delete recording");
        return;
    }
    const String route = recordingSidecarPath(name, ".ROUTE");
    const String metadata = recordingSidecarPath(name, ".AGENT.JSON");
    if (storage_->exists(route.c_str()) && !storage_->remove(route.c_str())) {
        Serial.println("[WEB] Could not delete recording route sidecar");
    }
    if (storage_->exists(metadata.c_str()) &&
        !storage_->remove(metadata.c_str())) {
        Serial.println("[WEB] Could not delete recording metadata sidecar");
    }
    JsonDocument response;
    response["ok"] = true;
    sendJson(client, response);
}

bool WebService::safeRecordingName(const String& name) const
{
    if (name.length() == 0 || name.length() > 96 || !isWav(name) ||
        name.indexOf("..") >= 0 || name.indexOf('/') >= 0 ||
        name.indexOf('\\') >= 0 || name.indexOf('"') >= 0 ||
        name.indexOf('\r') >= 0 || name.indexOf('\n') >= 0) {
        return false;
    }
    return true;
}

bool WebService::normalizeRecordingName(const String& input,
                                        String& name) const
{
    name = input;
    name.trim();
    String lower = name;
    lower.toLowerCase();
    if (lower.endsWith(".wav")) {
        name.remove(name.length() - 4);
        name.trim();
    }
    if (name.length() == 0 || name.length() > 32) {
        return false;
    }
    for (std::size_t index = 0; index < name.length(); ++index) {
        const char character = name[index];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_' || character == ' ')) {
            return false;
        }
    }
    name.toUpperCase();
    name += ".WAV";
    return true;
}

}  // namespace cardputer_recorder
