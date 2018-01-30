#ifdef RESIDUE_HAS_CURL
#include <curl/curl.h>
#else
#include <boost/asio.hpp>
#endif
#include "include/log.h"
#include "src/net/http-client.h"
#include "src/net/url.h"
#include "src/core/residue-exception.h"

using namespace residue;

#ifdef RESIDUE_HAS_CURL

std::size_t writeCallback(void* contents, std::size_t size, std::size_t nmemb, std::string* s) {
    std::size_t newLength = size*nmemb;
    std::size_t oldLength = s->size();
    try {
        s->resize(oldLength + newLength);
    } catch (std::bad_alloc& e) {
        //handle memory problem
        return 0;
    }

    std::copy((char*)contents, (char*)contents + newLength, s->begin() + oldLength);
    return size * nmemb;
}

std::string HttpClient::fetchUrlContents(const std::string& url)
{
    std::string result;

    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURL* curl = curl_easy_init();

    if (curl != nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, url.data());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, std::string("Residue/" +
                                                              std::string(RESIDUE_VERSION_MAJOR) +
                                                              std::string(RESIDUE_VERSION_MINOR)).data());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            RLOG(ERROR) << "curl_easy_perform() failed: " << curl_easy_strerror(res);
        }

        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return result;
}

#else
std::string HttpClient::fetchUrlContents(const std::string& url)
{

    Url parsedUrl(url);
    if (!parsedUrl.isValid()) {
        throw ResidueException("Invalid URL " + url);
    }

    if (!parsedUrl.isHttp()) {
        throw ResidueException("Only http URLs supported. Use libcurl if you want to get url contents from other protocols");
    }

    using boost::asio::ip::tcp;
    try {

        boost::asio::io_service io_service;

        // Get a list of endpoints corresponding to the server name.
        tcp::resolver resolver(io_service);
        tcp::resolver::query query(parsedUrl.host(), parsedUrl.port());
        tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        // Try each endpoint until we successfully establish a connection.
        tcp::socket socket(io_service);
        boost::asio::connect(socket, endpoint_iterator);

        boost::asio::streambuf request;
        std::ostream requestStream(&request);
        requestStream << "GET " << parsedUrl.path() << " HTTP/1.0\r\n";
        requestStream << "Host: " << parsedUrl.host() << "\r\n";
        requestStream << "Accept: */*\r\n";
        // Form the request. We specify the "Connection: close" header so that the
        // server will close the socket after transmitting the response. This will
        // allow us to treat all data up until the EOF as the content.
        requestStream << "Connection: close\r\n\r\n";

        // Send the request.
        boost::asio::write(socket, request);

        // Read the response status line. The response streambuf will automatically
        // grow to accommodate the entire line. The growth may be limited by passing
        // a maximum size to the streambuf constructor.
        boost::asio::streambuf response;
        boost::asio::read_until(socket, response, "\r\n");

        // Check that response is OK.
        std::istream responseStream(&response);
        std::string httpVersion;
        responseStream >> httpVersion;
        unsigned int statusCode;
        responseStream >> statusCode;
        std::string statusMessage;
        std::getline(responseStream, statusMessage);

        if (!responseStream || httpVersion.substr(0, 5) != "HTTP/") {
          throw ResidueException("Invalid response");
        }

        if (statusCode != 200) {
            throw ResidueException("Response returned with status code " + std::to_string(statusCode));
        }

        // Read the response headers, which are terminated by a blank line.
        boost::asio::read_until(socket, response, "\r\n\r\n");

        std::stringstream ss;
        // Process the response headers.
        std::string header;
        while (std::getline(responseStream, header) && header != "\r") {
          // ignore header
          // ss << header << "\n";
        }
        // ss << "\n";


        // Write whatever content we already have to output.
        if (response.size() > 0) {
          ss << &response;
        }

        // Read until EOF, writing data to output as we go.
        boost::system::error_code error;
        while (boost::asio::read(socket, response, boost::asio::transfer_at_least(1), error)) {
          ss << &response;
        }

        if (error != boost::asio::error::eof) {
          throw ResidueException(error.message());
        }
        return ss.str();
    }
    catch (std::exception& e) {
      RLOG(ERROR) << "Exception while getting URL (" << url << "): " << e.what() << "\n";
    }
    return "";
}
#endif