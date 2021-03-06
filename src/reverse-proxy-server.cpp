#include <map>
#include <string>
#include <fstream>
#include <string.h>
#include <getopt.h>

#include <mongoose.h>
#include <json.h>
#include <curl/curl.h>

class ReverseProxyServer {
public:
    ReverseProxyServer(const char* f_MappingFile);
    ~ReverseProxyServer();
    const std::map<std::string, std::string>& getMappings();
    void makeHttpRequest(const std::string& port, const std::string& path, struct mg_connection *conn);

private:
    void _readMappings(const char*);

    std::map<std::string, std::string> _mappings;
    CURL *curl_handle;
};

ReverseProxyServer::ReverseProxyServer(const char* f_MappingFile)
{
    //Initialize curl
    curl_global_init(CURL_GLOBAL_SSL);

    //Creating CURL handle here. Reusing it improves performance
    curl_handle = curl_easy_init();

    _readMappings(f_MappingFile);
}

ReverseProxyServer::~ReverseProxyServer()
{
    curl_global_cleanup ();
}

const std::map<std::string, std::string>& ReverseProxyServer::getMappings()
{
    return _mappings;
}

void ReverseProxyServer::_readMappings(const char* f_MappingFile)
{
    Json::Value mainObject;
    std::ifstream ifs(f_MappingFile);
    try {
        ifs >> mainObject;
    } catch (std::runtime_error e) {
        fprintf(stderr, "Error when reading %s, %s\n", f_MappingFile, e.what());
    }

    if (!mainObject.isObject())
        return;

    auto items = mainObject.getMemberNames();
    for (const auto& name : items) {
        _mappings[name] = mainObject[name].asString();
        //fprintf(stderr, "Reverse Proxy. URL: %s -> Port %s\n", name.c_str(), _mappings[name].c_str());
    }
}

void ReverseProxyServer::makeHttpRequest(const std::string& port, const std::string& path, struct mg_connection *conn)
{
    if(!curl_handle) {
        mg_send_status(conn, 500);
        return;
    }

    //Set URI
    //URL Encode the path
    std::stringstream ss(path);
    std::string item;
    std::string encoded_path("");
    while (std::getline(ss, item, '/')) {
        if(item.empty())
            continue;
        char* output = curl_easy_escape(curl_handle, item.c_str(), item.size());
        if(output) {
            encoded_path += "/" + std::string(output);
            curl_free(output);
        } else {
            encoded_path += "/" + item;
        }
    }
    if('/' == path[path.size()-1])
        encoded_path += "/";

    if (encoded_path.empty()) {
        encoded_path = "/";
    }

    std::string uri("localhost:" + port + encoded_path);
    std::string queryString = std::string(conn->query_string ? conn->query_string : "");
    if(!queryString.empty())
        uri += "?" + queryString;
    //printf("URI %s\n", uri.c_str());
    curl_easy_setopt(curl_handle, CURLOPT_URL, uri.c_str());

    //Set the method
    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, conn->request_method);

    typedef size_t fwrite_t(void*, size_t, size_t, void*);

    auto receiveData = [] (void *buffer, size_t size, size_t nmemb, void *userp) -> size_t {
        auto conn = (mg_connection*)userp;
        size_t realsize = size * nmemb;
        mg_send_data(conn, buffer, realsize);
        //std::string data((char*)buffer, realsize);
        //printf("Data: %ld %s", realsize, data.c_str());
        return realsize;
    };

    auto receiveHeader = [] (void *buffer, size_t size, size_t nmemb, void *userp) -> size_t {
        static const char* statusPrefix = "HTTP/1.1 ";
        static const char* transferEncodingPrefix = "Transfer-Encoding: ";
        static const char* contentLengthPrefix = "Content-Length: ";
        auto conn = (mg_connection*)userp;
        auto sbuf = (const char*)buffer;
        auto totalSize = size * nmemb;

        if ((strstr(sbuf, transferEncodingPrefix) == sbuf) ||
            (strstr(sbuf, contentLengthPrefix) == sbuf) ){
            //Ignore Transfer-Encoding & Content-Length headers as they get added by Mongoose.
            return totalSize;
        }
        if (strstr(sbuf, statusPrefix) == sbuf) {
            int code(500);
            sscanf(sbuf, "HTTP/1.1 %d", &code);
            //printf("Code %d", code);
            if(100 != code) //Ignore 100-Continue status as it is internally generated by Mongoose
                mg_send_status(conn, code);
            return totalSize;
        }

        std::string headerStr(sbuf, totalSize);
        //Remove any "\r\n"
        size_t pos = headerStr.find("\r\n");
        if(pos != std::string::npos)
            headerStr.erase(pos, strlen("\r\n"));

        size_t indexSep = headerStr.find(':');
        if(std::string::npos == indexSep) //Not a valid header
            return totalSize;
        //printf("Header %s %s", headerStr.substr(0, indexSep).c_str(), headerStr.substr(indexSep+2).c_str());
        mg_send_header(conn, headerStr.substr(0, indexSep).c_str(), headerStr.substr(indexSep+2).c_str());
        return totalSize;
    };

    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION,  (fwrite_t*)receiveData);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, conn);

    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, (fwrite_t*)receiveHeader);
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, conn);

    //Enable chunked transfer decoding by libcurl so that we do not send size of chunk again to caller
    curl_easy_setopt(curl_handle, CURLOPT_HTTP_TRANSFER_DECODING, 1L);

    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 0L);

    //Set headers
    struct curl_slist *headers=NULL;
    std::vector<std::pair<std::string, std::string> > incoming_headers;
    for(auto i = 0; i < conn->num_headers; ++i) {
        incoming_headers.push_back(std::make_pair(conn->http_headers[i].name, conn->http_headers[i].value));
        std::string str(std::string(conn->http_headers[i].name) + ": " + conn->http_headers[i].value);
        headers = curl_slist_append(headers, str.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    }

    //Set the body, if available
    if (conn->content_len) {
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, conn->content_len);
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, conn->content);
    }

    /* Perform the request, res will get the return code */
    CURLcode res = curl_easy_perform(curl_handle);
    if(res != CURLE_OK)
        mg_send_status(conn, 500);

    if(headers)
        curl_slist_free_all(headers); /* free the header list */
}

static int ev_handler(struct mg_connection *conn, enum mg_event ev) {
    if (ev == MG_AUTH)
        return MG_TRUE;
    if (ev != MG_REQUEST)
        return MG_FALSE;

    //printf("Servicing HTTP request [%s] %s\n", conn->request_method, conn->uri);

    ReverseProxyServer* reverseProxyServer = reinterpret_cast<ReverseProxyServer*>(conn->server_param);

    const auto& mappings = reverseProxyServer->getMappings();

    for (const auto& item : mappings) {
        if (strstr(conn->uri, item.first.c_str()) != conn->uri)
            continue;

        reverseProxyServer->makeHttpRequest(item.second, std::string(conn->uri).substr(item.first.size()), conn);

        return MG_TRUE;
    }

    return MG_FALSE;
}

int main(int argc, char *argv[])
{

  auto usage = [=] {
        fprintf(stderr, "Usage: %s -f mapping-file \n\n", argv[0]);
        exit(EXIT_FAILURE);
    };

    int opt;
    char* mappingFile = nullptr;
    char* port = nullptr;
    while ((opt = getopt(argc, argv, "f:p:")) != -1) {
        switch (opt) {
        case 'f': 
            mappingFile = strdup(optarg); break;
            break;
        case 'p': 
            port = strdup(optarg); break;
            break;
        default: 
            usage();
        }
    }

    if (!mappingFile || !port)
        usage();


  ReverseProxyServer reverseProxyServer(mappingFile);

  struct mg_server *server;

  // Create and configure the server
  server = mg_create_server(&reverseProxyServer, ev_handler);
  mg_set_option(server, "listening_port", port);

  // Serve request. Hit Ctrl-C to terminate the program
  //printf("Starting on port %s\n", mg_get_option(server, "listening_port"));
  for (;;) {
    mg_poll_server(server, 1000);
  }

  // Cleanup, and free server instance
  mg_destroy_server(&server);

  return 0;
}

