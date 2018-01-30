//
//  log-request-handler.cc
//  Residue
//
//  Copyright © 2017 Muflihun Labs
//

#include "include/log.h"
#include "src/logging/log-request-handler.h"
#include "src/logging/log-request.h"
#include "src/logging/user-log-builder.h"
#include "src/core/configuration.h"
#include "src/tasks/client-integrity-task.h"

using namespace residue;

LogRequestHandler::LogRequestHandler(Registry* registry,
                                     el::LogBuilder* userLogBuilder) :
    RequestHandler(registry),
    m_userLogBuilder(static_cast<UserLogBuilder*>(userLogBuilder))
{
    DRVLOG(RV_DEBUG) << "LogRequestHandler " << this << " with registry " << m_registry;
}

LogRequestHandler::~LogRequestHandler()
{
    m_stopped = true;
    for (auto& t : m_backgroundWorkers) {
        t.join();
    }
}

void LogRequestHandler::start()
{
    m_stopped = false;

    //
    // Important note regarding multiple dispatch threads for developers
    // who wrongly think multiple threads = faster code = speed = happy client
    //
    // There is performance decision that we made, either server can process
    // log messages fast or respond to the client application fast. If we want to change number
    // of dispatch threads (i.e, NUM_OF_DISPATCH_THREADS > 1) we need to move
    // 'rawRequestLock' in processRawRequests() just outside the while loop to prevent
    // the crash and race conditions amongst dispatch threads.
    //
    // Also you should re-add processLimit variable and it's check as per git commit 9b1297
    // for it will help speed up process. We have removed it because we are going to use single
    // dispatch thread officially.
    //
    // There is not much benefit in increasing number of threads for many reasons, some of them
    // listed below:
    //
    //  1 - Each thread can process as much requests as m_rawRequests contains, no more than that
    //  2 - When lock is busy via lock_guard 'rawRequestLock', client cannot add more requests because
    //     of responseLock lock_guard
    //  3 - There are multiple clients connecting to the server at the same time and adding multiple
    //     requests, client should be responded as soon as possible. We do not want to hold other
    //     clients just because we want faster dispatch process.
    //  4 - There are other tunings that can be done via configuration that can increase the speed
    //     of the server and we do not need more than one thread for dispatch
    //
    static const int NUM_OF_DISPATCH_THREADS = 1;
    static int idx = 1;
    for (; idx <= NUM_OF_DISPATCH_THREADS; ++idx) {
        m_backgroundWorkers.push_back(std::thread([&]() {
            el::Helpers::setThreadName(std::string("LogDispatcher") + std::to_string(idx));
            while (!m_stopped) {
                processRequestQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }));
    }
}

void LogRequestHandler::handle(RawRequest&& rawRequest)
{
    m_session->writeStatusCode(Response::StatusCode::STATUS_OK);
    std::lock_guard<std::mutex> lock(*(m_queue.lock()));
    m_queue.push(std::move(rawRequest));
}

void LogRequestHandler::processRequestQueue()
{
    bool allowPlainRequest = m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_PLAIN_LOG_REQUEST);
    bool compressionEnabled = m_registry->configuration()->hasFlag(Configuration::Flag::COMPRESSION);

#ifdef RESIDUE_PROFILING
    unsigned long m_timeTaken;
    RESIDUE_PROFILE_START(t_process_queue);
    std::size_t totalRequests = 0; // 1 for 1 request so for bulk of 50 this will be 50
#endif
    std::size_t total = m_queue.size();
    // don't use while as queue can get filled up during this time in some cases (even though we have concept of switching the queue)
    for (std::size_t i = 0; i < total; ++i) {

        if (m_registry->configuration()->dispatchDelay() > 0) {
            // We do not want to hold the client for m_backgroundWorkerMutex lock
            std::this_thread::sleep_for(std::chrono::milliseconds(m_registry->configuration()->dispatchDelay()));
        }

#if RESIDUE_DEBUG
        DRVLOG(RV_CRAZY) << "-----============= [ BEGIN ] =============-----";
#endif
        LogRequest request(m_registry->configuration());
        RawRequest rawRequest = m_queue.pull();

        RequestHandler::handle(std::move(rawRequest), &request, allowPlainRequest ?
                                   Request::StatusCode::CONTINUE : Request::StatusCode::BAD_REQUEST, false, false, compressionEnabled);

        if ((!request.isValid() && !request.isBulk())
                || request.statusCode() != Request::StatusCode::CONTINUE) {
            RVLOG(RV_ERROR) << "Failed: " << request.errorText();
            return;
        }

        if (request.isBulk()) {
            if (m_registry->configuration()->hasFlag(Configuration::ALLOW_BULK_LOG_REQUEST)) {
                // Create bulk request items
                unsigned int itemCount = 0U;
                JsonObject::Json j = request.jsonObject().root();
                unsigned long lastClientValidation = Utils::now();
                Client* currentClient = request.client();
                std::string lastKnownClientId = request.clientId();
                DRVLOG(RV_DEBUG) << "Request client: " << currentClient;
                bool forceClientValidation = true;
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if (itemCount == m_registry->configuration()->maxItemsInBulk()) {
                        RLOG(ERROR) << "Maximum number of bulk requests reached. Ignoring the rest of items in bulk";
                        break;
                    }
                    std::string requestItemStr(std::move(it->dump()));
                    LogRequest requestItem(m_registry->configuration());
                    requestItem.deserialize(std::move(requestItemStr));
                    if (requestItem.isValid()) {
                        requestItem.setIpAddr(request.ipAddr());
                        requestItem.setDateReceived(request.dateReceived());
                        if (!forceClientValidation
                                && m_registry->clientIntegrityTask() != nullptr
                                && lastClientValidation <= m_registry->clientIntegrityTask()->lastExecution()) {
                            forceClientValidation = true;
                            RLOG(INFO) << "Re-forcing client validation after client integrity task execution";
                            RLOG(DEBUG) << "[client: " << currentClient << "] => request client_id: [" << request.clientId() << "], last known client ID: [" << lastKnownClientId << "]";
                            // we invalidate the pointers as the may be pointing to invalid memory now
                            // so we will force the request to find the client
                            currentClient = nullptr;
                            requestItem.setClient(nullptr);
                            requestItem.setClientId(lastKnownClientId);
                            lastClientValidation = Utils::now();
                        }
                        if (processRequest(&requestItem, &currentClient, forceClientValidation)) {
                            lastKnownClientId = currentClient != nullptr ? currentClient->id() : "";
                            forceClientValidation = false;
                        } else {
                            // force next client validation if process is unsuccessful
                            forceClientValidation = true;
                        }
                        itemCount++;
#ifdef RESIDUE_PROFILING
                        totalRequests++;
#endif
                    } else {
                        RLOG(ERROR) << "Invalid request in bulk.";
                    }
                }
            } else {
                RLOG(ERROR) << "Bulk requests are not allowed";
            }
        } else {

            if (request.client() != nullptr) {
                request.setClientId(request.client()->id());
            }
            processRequest(&request, nullptr, true);
#ifdef RESIDUE_PROFILING
            totalRequests++;
#endif
        }

#if RESIDUE_DEBUG
        DRVLOG(RV_CRAZY) << "-----============= [ ✓ ] =============-----";
#endif
    }

#ifdef RESIDUE_PROFILING
    RESIDUE_PROFILE_END(t_process_queue, m_timeTaken);
    float timeTakenInSec = static_cast<float>(m_timeTaken / 1000.0f);
    RLOG_IF(total > 0, DEBUG) << "Took " << timeTakenInSec << " s to process the queue of "
                                   << total << " items (" << totalRequests << " requests). Average: "
                                   << (static_cast<float>(m_timeTaken) / static_cast<float>(total)) << " ms/item ["
                                   << (static_cast<float>(m_timeTaken) / static_cast<float>(totalRequests)) << " ms/request]";
    DRVLOG_IF(!m_queue.backlogEmpty(), RV_DEBUG) << m_queue.backlogSize() << " items have been added to this queue in the mean time";
#endif

    m_queue.switchContext();
}

bool LogRequestHandler::processRequest(LogRequest* request, Client** clientRef, bool forceCheck)
{

    bool bypassChecks = !forceCheck && clientRef != nullptr && *clientRef != nullptr;
#if RESIDUE_DEBUG
    DRVLOG(RV_DEBUG) << "Force check: " << forceCheck << ", clientRef: " << clientRef << ", *clientRef: "
                     << (clientRef == nullptr ? "N/A" : *clientRef == nullptr ? "null" : (*clientRef)->id())
                     << ", bypassChecks: " << bypassChecks;
#endif
    Client* client = clientRef != nullptr && *clientRef != nullptr ? *clientRef : request->client();

    if (client == nullptr) {
        if (m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_PLAIN_LOG_REQUEST)
                && (
                    // following || is: see if logger is unknown.. this line implies unknown loggers allow plain log requests but whether server allows or not is a different story
                    (m_registry->configuration()->hasLoggerFlag(request->loggerId(), Configuration::Flag::ALLOW_PLAIN_LOG_REQUEST))
                        || (!m_registry->configuration()->isKnownLogger(request->loggerId()) && m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_UNKNOWN_LOGGERS))
                    )
                && !request->clientId().empty()) {
            // Try to find client assuming plain JSON request
            client = m_registry->findClient(request->clientId());
        } else if (request->clientId().empty()) {
            RVLOG(RV_ERROR) << "Invalid request. No client ID found";
        }
    }

    if (clientRef != nullptr) {
        *clientRef = client;
    }

    if (client == nullptr) {
        RVLOG(RV_ERROR) << "Invalid request. No client found [" << request->clientId() << "]";
        if (m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_PLAIN_LOG_REQUEST)) {
            RVLOG(RV_ERROR) << "Check if logger has ALLOW_PLAIN_LOG_REQUEST option set and it contains client ID if needed.";
        }
        return false;
    }

    if (!bypassChecks && !client->isAlive(request->dateReceived())) {
        RLOG(ERROR) << "Invalid request. Client is dead";
        RLOG(DEBUG) << "Req received: " << request->dateReceived() << ", client created: " << client->dateCreated() << ", age: " << client->age() << ", result: " << client->dateCreated() + client->age();
        return false;
    }

    request->setClientId(client->id());
    request->setClient(client);

    if (!bypassChecks && client->isKnown()) {
        // take this opportunity to update the user for unknown logger

        // unknown loggers cannot be updated to specific user
        // without having a known client parent

        // make sure the current logger is unknown
        // otherwise we already know the user either from client or from logger itself
        if (m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_UNKNOWN_LOGGERS) // cannot be unknown logger unless server supports it
                && !m_registry->configuration()->isKnownLogger(request->loggerId())) {
            m_registry->configuration()->updateUnknownLoggerUserFromRequest(request->loggerId(), request);
        }
    }

    if (request->isValid()) {
        if (!bypassChecks && !isRequestAllowed(request)) {
            RLOG(WARNING) << "Ignoring log from unauthorized logger [" << request->loggerId() << "]";
            return false;
        }
        dispatch(request);
        return true;
    }
    return false;
}

void LogRequestHandler::dispatch(const LogRequest* request)
{
    m_userLogBuilder->setRequest(request);
    // %client_id
    el::Helpers::installCustomFormatSpecifier(el::CustomFormatSpecifier("%client_id",  std::bind(&LogRequestHandler::getClientId, this, std::placeholders::_1)));
    // %ip
    el::Helpers::installCustomFormatSpecifier(el::CustomFormatSpecifier("%ip",  std::bind(&LogRequestHandler::getIpAddr, this, std::placeholders::_1)));

    el::base::Writer(request->level(),
                     request->filename().c_str(),
                     request->lineNumber(),
                     request->function().c_str(),
                     el::base::DispatchAction::NormalLog,
                     request->verboseLevel()).construct(el::Loggers::getLogger(request->loggerId())) << request->msg();
    // Reset
    el::Helpers::uninstallCustomFormatSpecifier("%client_id");
    el::Helpers::uninstallCustomFormatSpecifier("%ip");

    m_userLogBuilder->setRequest(nullptr);
}

bool LogRequestHandler::isRequestAllowed(const LogRequest* request) const
{
    Client* client = request->client();
    if (client == nullptr) {
        RLOG(DEBUG) << "Client may have expired";
        return false;
    }
    // Ensure flag is on
    bool allowed = m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_UNKNOWN_LOGGERS);
    if (!allowed) {
        // we're not allowed to use unknown loggers. we make sure the current logger is actually unknown.
        allowed = m_registry->configuration()->isKnownLogger(request->loggerId());
    }
    if (allowed) {
         // We do not allow users to log using residue internal logger
        allowed = request->loggerId() != RESIDUE_LOGGER_ID;
    }
    if (allowed) {
         // Logger is blacklisted
        allowed = !m_registry->configuration()->isBlacklisted(request->loggerId());
    }
    if (allowed) {
        // Invalid token (either expired or not initialized)
        allowed = client->isValidToken(request->loggerId(), request->token(), m_registry, request->dateReceived());

        if (!allowed) {
            RLOG(WARNING) << "Token expired";
        }
    }
    return allowed;
}