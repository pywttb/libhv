#include "handler.h"

#include <thread>   // import std::thread
#include <chrono>   // import std::chrono

#include "hbase.h"
#include "htime.h"
#include "hfile.h"
#include "hstring.h"
#include "EventLoop.h" // import setTimeout, setInterval

int Handler::preprocessor(HttpRequest* req, HttpResponse* resp) {
    // printf("%s:%d\n", req->client_addr.ip.c_str(), req->client_addr.port);
    // printf("%s\n", req->Dump(true, true).c_str());

    // cors
    resp->headers["Access-Control-Allow-Origin"] = "*";
    if (req->method == HTTP_OPTIONS) {
        resp->headers["Access-Control-Allow-Origin"] = req->GetHeader("Origin", "*");
        resp->headers["Access-Control-Allow-Methods"] = req->GetHeader("Access-Control-Request-Method", "OPTIONS, HEAD, GET, POST, PUT, DELETE, PATCH");
        resp->headers["Access-Control-Allow-Headers"] = req->GetHeader("Access-Control-Request-Headers", "Content-Type");
        return HTTP_STATUS_NO_CONTENT;
    }

    // Unified verification request Content-Type?
    // if (req->content_type != APPLICATION_JSON) {
    //     return response_status(resp, HTTP_STATUS_BAD_REQUEST);
    // }

    // Deserialize request body to json, form, etc.
    req->ParseBody();

    // Unified setting response Content-Type?
    resp->content_type = APPLICATION_JSON;

#if 0
    // authentication sample code
    if (strcmp(req->path.c_str(), "/login") != 0) {
        string token = req->GetHeader("token");
        if (token.empty()) {
            response_status(resp, 10011, "Miss token");
            return HTTP_STATUS_UNAUTHORIZED;
        }
        else if (strcmp(token.c_str(), "abcdefg") != 0) {
            response_status(resp, 10012, "Token wrong");
            return HTTP_STATUS_UNAUTHORIZED;
        }
        return HTTP_STATUS_UNFINISHED;
    }
#endif

    return HTTP_STATUS_UNFINISHED;
}

int Handler::postprocessor(HttpRequest* req, HttpResponse* resp) {
    // printf("%s\n", resp->Dump(true, true).c_str());
    return resp->status_code;
}

int Handler::errorHandler(const HttpContextPtr& ctx) {
    int error_code = ctx->response->status_code;
    return response_status(ctx, error_code);
}

int Handler::largeFileHandler(const HttpContextPtr& ctx) {
    std::thread([ctx](){
        ctx->writer->Begin();
        std::string filepath = ctx->service->document_root + ctx->request->Path();
        HFile file;
        if (file.open(filepath.c_str(), "rb") != 0) {
            ctx->writer->WriteStatus(HTTP_STATUS_NOT_FOUND);
            ctx->writer->WriteHeader("Content-Type", "text/html");
            ctx->writer->WriteBody("<center><h1>404 Not Found</h1></center>");
            ctx->writer->End();
            return;
        }
        http_content_type content_type = CONTENT_TYPE_NONE;
        const char* suffix = hv_suffixname(filepath.c_str());
        if (suffix) {
            content_type = http_content_type_enum_by_suffix(suffix);
        }
        if (content_type == CONTENT_TYPE_NONE || content_type == CONTENT_TYPE_UNDEFINED) {
            content_type = APPLICATION_OCTET_STREAM;
        }
        size_t filesize = file.size();
        ctx->writer->WriteHeader("Content-Type", http_content_type_str(content_type));
        ctx->writer->WriteHeader("Content-Length", filesize);
        // ctx->writer->WriteHeader("Transfer-Encoding", "chunked");
        ctx->writer->EndHeaders();

        char* buf = NULL;
        int len = 4096; // 4K
        SAFE_ALLOC(buf, len);
        size_t total_readbytes = 0;
        int last_progress = 0;
        int sendbytes_per_ms = 1024; // 1KB/ms = 1MB/s = 8Mbps
        int sleep_ms_per_send = len / sendbytes_per_ms; // 4ms
        int sleep_ms = sleep_ms_per_send;
        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time;
        while (total_readbytes < filesize) {
            size_t readbytes = file.read(buf, len);
            if (readbytes <= 0) {
                // read file error!
                ctx->writer->close();
                break;
            }
            int nwrite = ctx->writer->WriteBody(buf, readbytes);
            if (nwrite < 0) {
                // disconnected!
                break;
            } else if (nwrite == 0) {
                // send too fast or peer recv too slow
                // reduce speed of send
                sleep_ms *= 2;
                // size_t write_backlog = hio_write_bufsize(ctx->writer->io());
            } else {
                // restore speed of send
                sleep_ms = sleep_ms_per_send;
            }
            total_readbytes += readbytes;
            int cur_progress = total_readbytes * 100 / filesize;
            if (cur_progress > last_progress) {
                // printf("<< %s progress: %ld/%ld = %d%%\n",
                //     ctx->request->path.c_str(), (long)total_readbytes, (long)filesize, (int)cur_progress);
                last_progress = cur_progress;
            }
            end_time += std::chrono::milliseconds(sleep_ms);
            std::this_thread::sleep_until(end_time);
        }
        ctx->writer->End();
        SAFE_FREE(buf);
        // auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
        // printf("<< %s taked %ds\n", ctx->request->path.c_str(), (int)elapsed_time.count());
    }).detach();
    return HTTP_STATUS_UNFINISHED;
}

int Handler::sleep(const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
    writer->WriteHeader("X-Response-tid", hv_gettid());
    unsigned long long start_ms = gettimeofday_ms();
    writer->response->Set("start_ms", start_ms);
    std::string strTime = req->GetParam("t", "1000");
    if (!strTime.empty()) {
        int ms = atoi(strTime.c_str());
        if (ms > 0) {
            hv_delay(ms);
        }
    }
    unsigned long long end_ms = gettimeofday_ms();
    writer->response->Set("end_ms", end_ms);
    writer->response->Set("cost_ms", end_ms - start_ms);
    response_status(writer, 0, "OK");
    return 200;
}

int Handler::setTimeout(const HttpContextPtr& ctx) {
    unsigned long long start_ms = gettimeofday_ms();
    ctx->set("start_ms", start_ms);
    std::string strTime = ctx->param("t", "1000");
    if (!strTime.empty()) {
        int ms = atoi(strTime.c_str());
        if (ms > 0) {
            hv::setTimeout(ms, [ctx, start_ms](hv::TimerID timerID){
                unsigned long long end_ms = gettimeofday_ms();
                ctx->set("end_ms", end_ms);
                ctx->set("cost_ms", end_ms - start_ms);
                response_status(ctx, 0, "OK");
            });
        }
    }
    return HTTP_STATUS_UNFINISHED;
}

int Handler::query(const HttpContextPtr& ctx) {
    // scheme:[//[user[:password]@]host[:port]][/path][?query][#fragment]
    // ?query => HttpRequest::query_params
    for (auto& param : ctx->params()) {
        ctx->set(param.first.c_str(), param.second);
    }
    response_status(ctx, 0, "OK");
    return 200;
}

int Handler::kv(HttpRequest* req, HttpResponse* resp) {
    if (req->content_type != APPLICATION_URLENCODED) {
        return response_status(resp, HTTP_STATUS_BAD_REQUEST);
    }
    resp->content_type = APPLICATION_URLENCODED;
    resp->kv = req->GetUrlEncoded();
    resp->SetUrlEncoded("int", 123);
    resp->SetUrlEncoded("float", 3.14);
    resp->SetUrlEncoded("string", "hello");
    return 200;
}

int Handler::json(HttpRequest* req, HttpResponse* resp) {
    if (req->content_type != APPLICATION_JSON) {
        return response_status(resp, HTTP_STATUS_BAD_REQUEST);
    }
    resp->content_type = APPLICATION_JSON;
    resp->json = req->GetJson();
    resp->json["int"] = 123;
    resp->json["float"] = 3.14;
    resp->json["string"] = "hello";
    return 200;
}

int Handler::form(HttpRequest* req, HttpResponse* resp) {
    if (req->content_type != MULTIPART_FORM_DATA) {
        return response_status(resp, HTTP_STATUS_BAD_REQUEST);
    }
    resp->content_type = MULTIPART_FORM_DATA;
    resp->form = req->GetForm();
    resp->SetFormData("int", 123);
    resp->SetFormData("float", 3.14);
    resp->SetFormData("string", "hello");
    // resp->SetFormFile("file", "test.jpg");
    return 200;
}

int Handler::grpc(HttpRequest* req, HttpResponse* resp) {
    if (req->content_type != APPLICATION_GRPC) {
        return response_status(resp, HTTP_STATUS_BAD_REQUEST);
    }
    // parse protobuf
    // ParseFromString(req->body);
    // resp->content_type = APPLICATION_GRPC;
    // serailize protobuf
    // resp->body = SerializeAsString(xxx);
    response_status(resp, 0, "OK");
    return 200;
}

int Handler::test(const HttpContextPtr& ctx) {
    ctx->setContentType(ctx->type());
    ctx->set("bool", ctx->get<bool>("bool"));
    ctx->set("int", ctx->get<int>("int"));
    ctx->set("float", ctx->get<float>("float"));
    ctx->set("string", ctx->get("string"));
    response_status(ctx, 0, "OK");
    return 200;
}

int Handler::restful(const HttpContextPtr& ctx) {
    // RESTful /:field/ => HttpRequest::query_params
    // path=/group/:group_name/user/:user_id
    std::string group_name = ctx->param("group_name");
    std::string user_id = ctx->param("user_id");
    ctx->set("group_name", group_name);
    ctx->set("user_id", user_id);
    response_status(ctx, 0, "OK");
    return 200;
}

int Handler::login(const HttpContextPtr& ctx) {
    std::string username = ctx->get("username");
    std::string password = ctx->get("password");
    if (username.empty() || password.empty()) {
        response_status(ctx, 10001, "Miss username or password");
        return HTTP_STATUS_BAD_REQUEST;
    }
    else if (strcmp(username.c_str(), "admin") != 0) {
        response_status(ctx, 10002, "Username not exist");
        return HTTP_STATUS_BAD_REQUEST;
    }
    else if (strcmp(password.c_str(), "123456") != 0) {
        response_status(ctx, 10003, "Password wrong");
        return HTTP_STATUS_BAD_REQUEST;
    }
    else {
        ctx->set("token", "abcdefg");
        response_status(ctx, 0, "OK");
        return HTTP_STATUS_OK;
    }
}

int Handler::upload(const HttpContextPtr& ctx) {
    int status_code = 200;
    if (ctx->is(MULTIPART_FORM_DATA)) {
        status_code = ctx->request->SaveFormFile("file", "html/uploads/");
    } else {
        status_code = ctx->request->SaveFile("html/uploads/upload.txt");
    }
    return response_status(ctx, status_code);
}
