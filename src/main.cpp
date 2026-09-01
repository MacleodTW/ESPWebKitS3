#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <esp_https_server.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <WiFiClient.h>
#include <lwip/sockets.h>

// ---------------------------------------------------------
// Configure Custom SoftAP IP settings
// ---------------------------------------------------------
IPAddress apIP(10, 1, 1, 1);
IPAddress apSubnet(255, 255, 255, 0);
String AP_SSID = "PS_WiFi";
String AP_PASS = "password";
bool enable_ap_mode = true; // AP mode enabled by default

// Default Network Configuration (For STA connection)
String current_wifi_ssid = "";
String current_wifi_pass = "";
String upstream_dns = "0.0.0.0"; // 0.0.0.0 means use DHCP provided DNS from STA

// Global handles and buffers for SSL & UDP
httpd_handle_t server = NULL;
char *server_cert = nullptr;
char *server_key = nullptr;
size_t server_cert_len = 0;
size_t server_key_len = 0;
WiFiUDP udp; // UDP instance for the DNS server

#define CHUNK_SIZE (8*1024) // 8KB chunk size for file reading
#define DNS_PORT 53

// Reusable static buffer to avoid per-request heap allocation/fragmentation
static uint8_t file_chunk[CHUNK_SIZE];

// Storage for ESP DNS Dashboard filtering rules
std::vector<String> dns_block_list;
std::vector<String> dns_white_list;
std::map<String, String> dns_rewrite_list;

const char *DNS_CONFIG_FILE = "/dns_config.json";

// ---------------------------------------------------------
// Default List Structures (Applied on first boot or factory reset)
// ---------------------------------------------------------
const std::vector<String> default_block_list = {
    // Playstation
    "playstation",
    "sonyentertainmentnetwork",
    "ribob01",
    "akamai",
    // Youtube
    "youtube",
    "ggpht",
    "googlevideo",
    "yt.be",
    "ytimg.com",
    "yt3.googleusercontent.com"};

const std::vector<String> default_white_list = {
    // Playstation check network connection
    "ena.net.playstation.net",
    // Github
    "github.com",
};

const std::map<String, String> default_rewrite_list = {
    // User's guide
    {"manuals.playstation.net", "0.0.0.0"},
};

/**
 * @brief Save current DNS, AP, and WiFi rules to FFat storage
 */
void save_dns_config()
{
    JsonDocument doc;

    doc["enable_ap"] = enable_ap_mode;
    doc["wifi_ssid"] = current_wifi_ssid;
    doc["wifi_pass"] = current_wifi_pass;
    doc["upstream_dns"] = upstream_dns;

    JsonArray blockArr = doc["block"].to<JsonArray>();
    for (const auto &domain : dns_block_list)
        blockArr.add(domain);

    JsonArray whiteArr = doc["white"].to<JsonArray>();
    for (const auto &domain : dns_white_list)
        whiteArr.add(domain);

    JsonObject rewriteObj = doc["rewrite"].to<JsonObject>();
    for (const auto &pair : dns_rewrite_list)
        rewriteObj[pair.first] = pair.second;

    File file = LittleFS.open(DNS_CONFIG_FILE, FILE_WRITE);
    if (!file)
    {
        log_e("Failed to open DNS config for writing");
        return;
    }
    serializeJson(doc, file);
    file.close();
    log_i("DNS config saved successfully.");
}

/**
 * @brief Load DNS, AP, and WiFi rules from FFat storage on system boot
 */
void load_dns_config()
{
    File file = LittleFS.open(DNS_CONFIG_FILE, FILE_READ);
    if (!file)
    {
        log_i("No existing DNS config found, applying default structures.");

        // Apply default structures to memory
        dns_block_list = default_block_list;
        dns_white_list = default_white_list;
        dns_rewrite_list = default_rewrite_list;

        // Save these defaults to Flash immediately so the Web UI can fetch them
        save_dns_config();
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
        log_e("Failed to parse DNS config: %s", error.c_str());
        return;
    }

    if (doc["enable_ap"].is<bool>())
        enable_ap_mode = doc["enable_ap"].as<bool>();
    if (doc["wifi_ssid"].is<String>())
        current_wifi_ssid = doc["wifi_ssid"].as<String>();
    if (doc["wifi_pass"].is<String>())
        current_wifi_pass = doc["wifi_pass"].as<String>();
    if (doc["upstream_dns"].is<String>())
        upstream_dns = doc["upstream_dns"].as<String>();

    dns_block_list.clear();
    if (doc["block"].is<JsonArray>())
    {
        for (const char *domain : doc["block"].as<JsonArray>())
            dns_block_list.push_back(String(domain));
    }

    dns_white_list.clear();
    if (doc["white"].is<JsonArray>())
    {
        for (const char *domain : doc["white"].as<JsonArray>())
            dns_white_list.push_back(String(domain));
    }

    dns_rewrite_list.clear();
    if (doc["rewrite"].is<JsonObject>())
    {
        JsonObject rewriteObj = doc["rewrite"].as<JsonObject>();
        for (JsonPair kv : rewriteObj)
            dns_rewrite_list[String(kv.key().c_str())] = String(kv.value().as<const char *>());
    }

    log_i("Config loaded. AP Mode: %d, SSID: %s, Upstream DNS: %s", enable_ap_mode, current_wifi_ssid.c_str(), upstream_dns.c_str());
}

/**
 * @brief Helper function to read a file from FATFS into RAM.
 */
char *readCertToBuffer(const char *path, size_t *out_len)
{
    File file = LittleFS.open(path, FILE_READ);
    if (!file)
        return nullptr;

    size_t size = file.size();
    char *buffer = (char *)malloc(size + 1);
    if (!buffer)
    {
        file.close();
        return nullptr;
    }

    file.read((uint8_t *)buffer, size);
    buffer[size] = '\0';
    if (out_len)
        *out_len = size + 1;

    file.close();
    return buffer;
}

/**
 * @brief Determine the HTTP Content-Type based on the file extension.
 *        Uses a single strrchr() lookup instead of repeated strstr() scans.
 */
const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL)
        return "text/plain";

    if (strcmp(ext, ".html") == 0)
        return "text/html";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".js") == 0)
        return "application/javascript";
    if (strcmp(ext, ".json") == 0)
        return "application/json";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(ext, ".svg") == 0)
        return "image/svg+xml";
    // Support for binary and executable files
    if (strcmp(ext, ".bin") == 0 || strcmp(ext, ".elf") == 0)
        return "application/octet-stream";
    return "text/plain";
}

/**
 * @brief GET /api/dns - Return current settings, network states, and actual DNS IP
 */
esp_err_t api_dns_get_handler(httpd_req_t *req)
{
    log_d("API GET request path: %s", req->uri);

    JsonDocument doc;

    doc["enable_ap"] = enable_ap_mode;
    doc["wifi_ssid"] = current_wifi_ssid;
    doc["wifi_pass"] = current_wifi_pass;
    doc["upstream_dns"] = upstream_dns;
    doc["sta_ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "Not Connected";
    doc["actual_dns"] = (WiFi.status() == WL_CONNECTED) ? WiFi.dnsIP().toString() : "0.0.0.0";

    JsonArray blockArr = doc["block"].to<JsonArray>();
    for (const auto &domain : dns_block_list)
        blockArr.add(domain);

    JsonArray whiteArr = doc["white"].to<JsonArray>();
    for (const auto &domain : dns_white_list)
        whiteArr.add(domain);

    JsonObject rewriteObj = doc["rewrite"].to<JsonObject>();
    for (const auto &pair : dns_rewrite_list)
        rewriteObj[pair.first] = pair.second;

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief POST /api/dns - Receive JSON payload, update DNS rules and network configs
 */
esp_err_t api_dns_post_handler(httpd_req_t *req)
{
    log_d("API POST request path: %s", req->uri);

    char buf[1024];
    int ret, remaining = req->content_len;
    String json_data = "";

    while (remaining > 0)
    {
        if ((ret = httpd_req_recv(req, buf, min(remaining, (int)sizeof(buf) - 1))) <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            return ESP_FAIL;
        }
        buf[ret] = '\0';
        json_data += buf;
        remaining -= ret;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json_data);
    if (error)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool wifi_changed = false;
    bool ap_changed = false;

    if (doc["enable_ap"].is<bool>())
    {
        bool new_ap_mode = doc["enable_ap"].as<bool>();
        if (new_ap_mode != enable_ap_mode)
        {
            enable_ap_mode = new_ap_mode;
            ap_changed = true;
        }
    }

    if (doc["wifi_ssid"].is<String>() && doc["wifi_pass"].is<String>())
    {
        String new_ssid = doc["wifi_ssid"].as<String>();
        String new_pass = doc["wifi_pass"].as<String>();
        if (new_ssid != current_wifi_ssid || new_pass != current_wifi_pass)
        {
            current_wifi_ssid = new_ssid;
            current_wifi_pass = new_pass;
            wifi_changed = true;
        }
    }

    if (doc["upstream_dns"].is<String>())
        upstream_dns = doc["upstream_dns"].as<String>();

    if (doc["block"].is<JsonArray>())
    {
        dns_block_list.clear();
        for (const char *domain : doc["block"].as<JsonArray>())
            dns_block_list.push_back(String(domain));
    }
    if (doc["white"].is<JsonArray>())
    {
        dns_white_list.clear();
        for (const char *domain : doc["white"].as<JsonArray>())
            dns_white_list.push_back(String(domain));
    }
    if (doc["rewrite"].is<JsonObject>())
    {
        dns_rewrite_list.clear();
        JsonObject rewriteObj = doc["rewrite"].as<JsonObject>();
        for (JsonPair kv : rewriteObj)
            dns_rewrite_list[String(kv.key().c_str())] = String(kv.value().as<const char *>());
    }

    save_dns_config();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);

    // Apply AP Mode changes dynamically
    if (ap_changed)
    {
        if (enable_ap_mode)
        {
            log_i("Enabling AP Mode...");
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAPConfig(apIP, apIP, apSubnet);
            WiFi.softAP(AP_SSID, AP_PASS);
        }
        else
        {
            log_i("Disabling AP Mode...");
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
        }
    }

    // Reconnect STA WiFi if settings changed
    if (wifi_changed)
    {
        log_i("WiFi credentials changed, attempting to reconnect...");
        WiFi.disconnect();
        WiFi.begin(current_wifi_ssid.c_str(), current_wifi_pass.c_str());

        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 20)
        {
            delay(500);
            Serial.print(".");
            retry++;
        }
        if (retry == 20)
        {
            WiFi.disconnect();
            log_w("Failed to connect to STA WiFi.");
        }
  
    }

    return ESP_OK;
}

/**
 * @brief POST /api/factory_reset - Delete config file and restart ESP32
 */
esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    log_d("API POST (Reset) request path: %s", req->uri);
    log_w("Factory reset requested! Deleting config and rebooting...");

    // Remove the saved configuration file from Flash
    if (LittleFS.exists(DNS_CONFIG_FILE))
    {
        LittleFS.remove(DNS_CONFIG_FILE);
        log_i("Configuration file removed.");
    }

    // Send success response back to the browser
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);

    // Give the HTTP response a moment to be sent before pulling the plug
    delay(1000);
    ESP.restart(); // Reboot the microcontroller

    return ESP_OK;
}

/**
 * @brief POST /api/payload/<filename> - Payload inject through TCP to client
 */
esp_err_t api_payload_post_handler(httpd_req_t *req)
{
    log_d("API POST Payload request path: %s", req->uri);

    // 1. Get filename from URI
    char filepath[128];
    strlcpy(filepath, req->uri, sizeof(filepath));
    
    char *query_ptr = strchr(filepath, '?');
    if (query_ptr != NULL) {
        *query_ptr = '\0';
    }

    String uri_str = String(filepath);
    String prefix = "/api/payload/";
    if (!uri_str.startsWith(prefix)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    
    String filename = uri_str.substring(prefix.length());

    // 2. Only accept .elf or .bin
    if (filename.length() == 0 || (!filename.endsWith(".elf") && !filename.endsWith(".bin"))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad payload name\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 3. Get file from LittleFS /payloads/
    String fs_path = "/payloads/" + filename;
    File file = LittleFS.open(fs_path, FILE_READ);
    if (!file || file.isDirectory()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"payload not found\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 4. Get client IP
    int sockfd = httpd_req_to_sockfd(req);
    char ip_str[INET6_ADDRSTRLEN] = {0};
    struct sockaddr_in6 client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    if (getpeername(sockfd, (struct sockaddr *)&client_addr, &addr_len) < 0) {
        file.close();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"no client address\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Handle IPv4 or IPv4-mapped IPv6
    if (client_addr.sin6_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
    } else if (client_addr.sin6_family == AF_INET6) {
        inet_ntop(AF_INET6, &client_addr.sin6_addr, ip_str, sizeof(ip_str));
        if (strncmp(ip_str, "::ffff:", 7) == 0) {
            memmove(ip_str, ip_str + 7, strlen(ip_str) - 6);
        }
    }

    // 5. Connect TCP to client 9021 port
    int port = 9021;
    WiFiClient tcpClient;
    tcpClient.setTimeout(15); 
    
    if (!tcpClient.connect(ip_str, port)) {
        file.close();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "502 Bad Gateway");
        String err_msg = "{\"ok\":false,\"error\":\"connect " + String(ip_str) + ":" + String(port) + " failed\"}";
        httpd_resp_send(req, err_msg.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 6. Write file to TCP Socket
    size_t sent = 0;
    uint8_t *buf = (uint8_t *)malloc(CHUNK_SIZE); 
    if (!buf) {
        file.close();
        tcpClient.stop();
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (file.available()) {
        size_t bytes_read = file.read(buf, CHUNK_SIZE);
        size_t bytes_written = tcpClient.write(buf, bytes_read);
        
        if (bytes_written != bytes_read) {
            tcpClient.stop();
            free(buf);
            file.close();
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "502 Bad Gateway");
            String err_msg = "{\"ok\":false,\"error\":\"write failed after " + String(sent) + " bytes\"}";
            httpd_resp_send(req, err_msg.c_str(), HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        sent += bytes_written;
    }
    
    free(buf);
    file.close();
    tcpClient.stop();

    // 7. Return JSON successful format
    JsonDocument res_doc;
    res_doc["ok"] = true;
    res_doc["bytes"] = sent;
    res_doc["name"] = filename;
    res_doc["to"] = String(ip_str) + ":" + String(port);
    
    String response;
    serializeJson(res_doc, response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    
    return ESP_OK;
}

/**
 * @brief HTTP GET Handler to serve static files dynamically from LittleFS.
 */
esp_err_t file_get_handler(httpd_req_t *req)
{
    log_d("Web GET request path: %s", req->uri);

    char filepath[128];

    // 1. Copy the original URI first
    strlcpy(filepath, req->uri, sizeof(filepath));

    // 2. Filter out query string parameters, e.g., "?v=final"
    char *query_ptr = strchr(filepath, '?');
    if (query_ptr != NULL)
    {
        *query_ptr = '\0'; // Replace '?' with string terminator
    }

    // 3. Rewrite path /document/xx/ps5/ to /index.html
    if (strncmp(filepath, "/document/", 10) == 0)
    {
        char *lang_end = strchr(filepath + 10, '/');
        if (lang_end != NULL)
        {
            char temp[128];            
            if (strncmp(lang_end, "/ps5/", 5) == 0 || strcmp(lang_end, "/ps5") == 0)
            {
                lang_end = strstr(filepath, "/ps5/");
                if (*(lang_end + 5) != 0)                
                    sprintf(filepath, "/%s", (lang_end + 5));
                else
                    strcpy(filepath, "/");
            }
            log_d("URI path rewritten to: %s", filepath);            
        }
    }

    // 4. Perform routing based on the clean path
    if (strcmp(filepath, "/") == 0)
    {
        strlcpy(filepath, "/index.html", sizeof(filepath));
    }
    else if (strcmp(filepath, "/dns") == 0)
    {
        strlcpy(filepath, "/dns.html", sizeof(filepath));
    }
    else
    {
        size_t len = strlen(filepath);
        if (len > 0 && filepath[len - 1] == '/')
        {
            strlcat(filepath, "index.html", sizeof(filepath));
            log_d("URI add default page: %s", filepath);
        }
    }

    File file = LittleFS.open(filepath, FILE_READ);
    if (!file || file.isDirectory())
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));

    size_t bytes_read;
    while ((bytes_read = file.read(file_chunk, CHUNK_SIZE)) > 0)
    {
        if (httpd_resp_send_chunk(req, (char *)file_chunk, bytes_read) != ESP_OK)
        {
            file.close();
            return ESP_FAIL;
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    file.close();

    return ESP_OK;
}

/**
 * @brief Configures and starts the HTTPS server.
 */
void start_https_server()
{
    server_cert = readCertToBuffer("/cert.pem", &server_cert_len);
    server_key = readCertToBuffer("/key.pem", &server_key_len);

    if (!server_cert || !server_key)
    {
        log_e("Certificates missing. Cannot start server.");
        return;
    }

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();

    // Updated variable names for ESP-IDF v5 (Arduino ESP32 v3.x)
    conf.cacert_pem = (const uint8_t *)server_cert;
    conf.cacert_len = server_cert_len;
    conf.prvtkey_pem = (const uint8_t *)server_key;
    conf.prvtkey_len = server_key_len;

    // Tune the underlying HTTP server for TLS and this application
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
    conf.httpd.stack_size = 8192;         // Larger stack for TLS handshake + chunked transfer
    conf.httpd.max_uri_handlers = 16;     // Avoid runtime realloc as handlers are added
    conf.httpd.max_resp_headers = 16;     // Headroom for Content-Type / Content-Length etc.

    // Define URI definitions
    httpd_uri_t api_get_uri = {.uri = "/api/dns", .method = HTTP_GET, .handler = api_dns_get_handler, .user_ctx = NULL};
    httpd_uri_t api_post_uri = {.uri = "/api/dns", .method = HTTP_POST, .handler = api_dns_post_handler, .user_ctx = NULL};
    httpd_uri_t api_reset_uri = {.uri = "/api/factory_reset", .method = HTTP_POST, .handler = api_factory_reset_handler, .user_ctx = NULL};
    
    //  Payload API
    httpd_uri_t api_payload_uri = {.uri = "/api/payload/*", .method = HTTP_POST, .handler = api_payload_post_handler, .user_ctx = NULL};

    httpd_uri_t file_uri = {.uri = "/*", .method = HTTP_GET, .handler = file_get_handler, .user_ctx = NULL};

    if (httpd_ssl_start(&server, &conf) == ESP_OK)
    {
        httpd_register_uri_handler(server, &api_get_uri);
        httpd_register_uri_handler(server, &api_post_uri);

        // Register reset URI
        httpd_register_uri_handler(server, &api_reset_uri);

        // Default ('/*') 
        httpd_register_uri_handler(server, &api_payload_uri);

        httpd_register_uri_handler(server, &file_uri);
        log_i("HTTPS server successfully started.");
    }
}

// ------------------------------------------------------------------
// DNS Proxy & Filtering Logic
// ------------------------------------------------------------------

/**
 * @brief Extract domain name string from raw DNS UDP packet
 */
String extract_domain(uint8_t *buffer, int len)
{
    String domain = "";
    int pos = 12; // Skip 12-byte DNS header
    while (pos < len)
    {
        uint8_t label_len = buffer[pos++];
        if (label_len == 0)
            break;
        if (domain.length() > 0)
            domain += ".";
        for (int i = 0; i < label_len && pos < len; i++)
        {
            domain += (char)buffer[pos++];
        }
    }
    return domain;
}

/**
 * @brief Forge and send a custom DNS response (A record) to the client
 */
void send_custom_dns_response(uint8_t *request, int req_len, IPAddress client_ip, uint16_t client_port, IPAddress resolve_ip)
{
    uint8_t response[512];
    memset(response, 0, 512);

    if (req_len > 512 - 16)
        return;
    memcpy(response, request, req_len);

    response[2] = 0x81;
    response[3] = 0x80;

    response[6] = 0x00;
    response[7] = 0x01;

    int pos = req_len;
    response[pos++] = 0xC0;
    response[pos++] = 0x0C;
    response[pos++] = 0x00;
    response[pos++] = 0x01;
    response[pos++] = 0x00;
    response[pos++] = 0x01;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x3C;
    response[pos++] = 0x00;
    response[pos++] = 0x04;

    response[pos++] = resolve_ip[0];
    response[pos++] = resolve_ip[1];
    response[pos++] = resolve_ip[2];
    response[pos++] = resolve_ip[3];

    udp.beginPacket(client_ip, client_port);
    udp.write(response, pos);
    udp.endPacket();
}

/**
 * @brief Proxy the DNS request to the upstream server and relay the answer back
 */
void forward_dns_query(uint8_t *request, int req_len, IPAddress client_ip, uint16_t client_port)
{
    IPAddress upstream;
    if (upstream_dns == "0.0.0.0" || upstream_dns == "")
    {
        upstream = WiFi.dnsIP(); // Use DHCP assigned DNS
    }
    else
    {
        upstream.fromString(upstream_dns);
    }

    if (upstream == IPAddress(0, 0, 0, 0))
        return; // No upstream available

    WiFiUDP proxyUdp;
    proxyUdp.begin(0);
    proxyUdp.beginPacket(upstream, 53);
    proxyUdp.write(request, req_len);
    proxyUdp.endPacket();

    unsigned long start = millis();
    while (millis() - start < 2000)
    {
        int res_len = proxyUdp.parsePacket();
        if (res_len > 0)
        {
            uint8_t res_buf[512];
            proxyUdp.read(res_buf, sizeof(res_buf));

            udp.beginPacket(client_ip, client_port);
            udp.write(res_buf, res_len);
            udp.endPacket();
            break;
        }
        delay(10);
    }
}

// ------------------------------------------------------------------
// Setup & Loop
// ------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(1000);

    log_i("Mounting FAT filesystem...");
    if (!LittleFS.begin(true))
    {
        log_e("FAT Mount Failed.");
        while (true)
            delay(1000);
    }

    load_dns_config();

    // Initialize WiFi Modes based on configuration
    if (enable_ap_mode)
    {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAPConfig(apIP, apIP, apSubnet);
        WiFi.softAP("PS_WiFi", "password");
        log_i("SoftAP started. SSID: PS_WiFi, IP: %s", WiFi.softAPIP().toString().c_str());
    }
    else
    {
        WiFi.mode(WIFI_STA);
        log_i("SoftAP is disabled by configuration.");
    }

    if (current_wifi_ssid.length() > 0)
    {
        log_i("Connecting to STA WiFi network: %s", current_wifi_ssid.c_str());
        WiFi.begin(current_wifi_ssid.c_str(), current_wifi_pass.c_str());

        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 20)
        {
            delay(500);
            Serial.print(".");
            retry++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            log_i("WiFi connected. STA IP Address: %s", WiFi.localIP().toString().c_str());
            log_i("Assigned DNS IP: %s", WiFi.dnsIP().toString().c_str());
        }
        else
        {
            WiFi.disconnect();
            log_w("Failed to connect to STA WiFi.");
        }
    }
    else
    {
        log_i("No STA WiFi configured. Running in standalone mode.");
    }

    start_https_server();

    // Start UDP DNS Server on port 53
    udp.begin(DNS_PORT);
    log_i("DNS Server listening on UDP port 53");
}

void loop()
{
    // Process incoming DNS packets
    int packetSize = udp.parsePacket();

    if (packetSize > 0 && packetSize <= 512)
    {
        uint8_t request[512];
        udp.read(request, sizeof(request));

        IPAddress client_ip = udp.remoteIP();
        uint16_t client_port = udp.remotePort();

        String domain = extract_domain(request, packetSize);
        if (domain.length() == 0)
            return;

        log_d("DNS Query: %s from %s", domain.c_str(), client_ip.toString().c_str());

        // 1. Check White List (Overrides all)
        bool whitelisted = false;
        for (const String &w : dns_white_list)
        {
            if (domain.indexOf(w) >= 0)
            {
                whitelisted = true;
                break;
            }
        }

        if (whitelisted)
        {
            log_d("DNS Action [Whitelist]: Forwarding %s", domain.c_str());
            forward_dns_query(request, packetSize, client_ip, client_port);
            return;
        }

        // 2. Check Rewrite List
        bool rewritten = false;
        for (const auto &pair : dns_rewrite_list)
        {
            if (domain.endsWith(pair.first))
            {
                IPAddress rewrite_ip;

                // If rule is 0.0.0.0, dynamically resolve to ESP32's own IP
                if (pair.second == "0.0.0.0")
                {
                    // Check if the client is querying from the SoftAP subnet
                    if (enable_ap_mode && client_ip[0] == apIP[0] && client_ip[1] == apIP[1] && client_ip[2] == apIP[2])
                    {
                        rewrite_ip = WiFi.softAPIP();
                    }
                    // Otherwise, if connected to a router, use the STA IP
                    else if (WiFi.status() == WL_CONNECTED)
                    {
                        rewrite_ip = WiFi.localIP();
                    }
                    // Fallback to SoftAP IP if STA is disconnected
                    else
                    {
                        rewrite_ip = WiFi.softAPIP();
                    }
                }
                else
                {
                    // Parse the standard IP string
                    rewrite_ip.fromString(pair.second);
                }

                log_d("DNS Action [Rewrite]: %s -> %s", domain.c_str(), rewrite_ip.toString().c_str());
                send_custom_dns_response(request, packetSize, client_ip, client_port, rewrite_ip);
                rewritten = true;
                break;
            }
        }
        if (rewritten)
            return;

        // 3. Check Block List
        bool blocked = false;
        for (const String &b : dns_block_list)
        {
            if (domain.indexOf(b) >= 0)
            {
                blocked = true;
                break;
            }
        }

        if (blocked)
        {
            log_d("DNS Action [Block]: Dropping %s", domain.c_str());
            send_custom_dns_response(request, packetSize, client_ip, client_port, IPAddress(0, 0, 0, 0));
            return;
        }

        // 4. Default action: Forward query
        log_d("DNS Action [Default]: Forwarding %s", domain.c_str());
        forward_dns_query(request, packetSize, client_ip, client_port);
    }
}
