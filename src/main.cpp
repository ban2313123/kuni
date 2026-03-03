#include <range/v3/action/insert.hpp>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/transform.hpp>


#include "AUI/Common/AByteBuffer.h"
#include "AUI/IO/AFileInputStream.h"
#include "AUI/Platform/Entry.h"
#include "AUI/Util/ASharedRaiiHelper.h"
#include "AUI/Util/kAUI.h"
#include "AppBase.h"
#include "telegram/TelegramClient.h"

namespace {

    constexpr auto LOG_TAG = "App";
    constexpr auto DAIRY_DIR = "dairy";

    AEventLoop gEventLoop;

    class App : public AppBase {
    public:
        App() {
            mTelegram->onEvent = [this](td::td_api::object_ptr<td::td_api::Object> event) {
                td::td_api::downcast_call(*event,
                                          [this](auto& u) { mAsync << this->handleTelegramEvent(std::move(u)); });
            };
        }

        [[nodiscard]] _<TelegramClient> telegram() const { return mTelegram; }


    protected:
        AFuture<> telegramPostMessage(int64_t chatId, AString text) override {
            co_await telegram()->sendQueryWithResult([&] {
                auto msg = td::td_api::make_object<td::td_api::sendMessage>();
                msg->chat_id_ = chatId;
                msg->input_message_content_ = [&] {
                    auto content = td::td_api::make_object<td::td_api::inputMessageText>();
                    content->text_ = [&] {
                        auto t = td::td_api::make_object<td::td_api::formattedText>();
                        t->text_ = text;
                        return t;
                    }();
                    return content;
                }();
                return msg;
            }());
        }

        AVector<AString> dairyRead() const override {
            APath dairyDir(DAIRY_DIR);
            if (!dairyDir.isDirectoryExists()) {
                return {};
            }
            AVector<AString> dairy;
            for (const auto& file: dairyDir.listDir()) {
                if (file.isRegularFileExists() && file.extension() == "md") {
                    dairy << AString::fromUtf8(AByteBuffer::fromStream(AFileInputStream(file)));
                }
            }
            return dairy;
        }

        void dairySave(const AString& message) override {
            APath dairyDir(DAIRY_DIR);
            dairyDir.makeDirs();
            auto dairyFile = dairyDir / "{}.md"_format(std::chrono::system_clock::now().time_since_epoch().count());
            AFileOutputStream(dairyFile) << message;
            ALogger::info("App") << "dairySave: " << dairyFile;
        }

    private:
        _<TelegramClient> mTelegram = _new<TelegramClient>();


        AFuture<> handleTelegramEvent(auto u) {
            TelegramClient::StubHandler{}(u);
            co_return;
        }

        AFuture<> handleTelegramEvent(td::td_api::updateNewMessage u) {
            int64_t userId = 0;
            td::td_api::downcast_call(*u.message_->sender_id_,
                                      aui::lambda_overloaded{
                                          [&](td::td_api::messageSenderUser& user) { userId = user.user_id_; },
                                          [&](auto&) {},
                                      });
            if (userId == 0) {
                co_return;
            }
            if (userId == mTelegram->myId()) {
                co_return;
            }
            auto chat = co_await mTelegram->sendQueryWithResult(
                td::td_api::make_object<td::td_api::getChat>(u.message_->chat_id_));
            auto notification = co_await [&]() -> AFuture<AString> {
                if (userId == u.message_->chat_id_) {
                    co_return "You received a direct message from {} (chat_id = {}):\n\n{}"_format(
                        chat->title_, chat->id_, to_string(u.message_->content_));
                }
                auto user =
                    co_await mTelegram->sendQueryWithResult(td::td_api::make_object<td::td_api::getUser>(userId));
                co_return "{} {} (user_id = {}) sent a message in group chat \"{}\" (chat_id = {}):\n\n{}"_format(
                    user->first_name_, user->last_name_, userId, chat->title_, chat->id_,
                    to_string(u.message_->content_));
            }();

            passNotificationToAI(
                std::move(notification),
                {
                    {
                        .name = "open",
                        .description = "Opens notification. Use this if you'd like to reply.",
                        .handler = [this, chatId = chat->id_](
                                       OpenAITools::Ctx ctx) { return llmuiOpenTelegramChat(ctx.tools, chatId); },
                    },

                });

            co_return;
        }

        void setOnline(bool online = true) {
            mTelegram->sendQuery(TelegramClient::toPtr(td::td_api::setOption(
                "online", TelegramClient::toPtr(td::td_api::optionValueBoolean(online)))));
    }

        AFuture<AString> llmuiOpenTelegramChat(OpenAITools& tools, int64_t chatId) {
            setOnline();
            mTelegram->sendQuery(TelegramClient::toPtr(td::td_api::openChat(chatId)));
            auto chatCloseMarker = ASharedRaiiHelper::make([this, self = shared_from_this(), chatId] {
                mTelegram->sendQuery(TelegramClient::toPtr(td::td_api::closeChat(chatId)));
                setOnline(false);
            });
            auto chat = co_await mTelegram->sendQueryWithResult(TelegramClient::toPtr(td::td_api::getChat(chatId)));
            AString result = "You opened the chat \"{}\" in Telegram. You see last messages:\n"_format(chat->title_);

            td::td_api::array<td::td_api::object_ptr<td::td_api::message>> messages;
            {
                const size_t targetMessageCount = chat->unread_count_ + 10;
                int64_t fromMessage = 0;
                while (messages.size() < targetMessageCount) {
                    auto response = co_await mTelegram->sendQueryWithResult(TelegramClient::toPtr(
                        td::td_api::getChatHistory(chatId, fromMessage, 0, glm::min(targetMessageCount - messages.size(), size_t(100)), false)));
                    if (response->messages_.empty()) {
                        result += "No messages found.";
                        goto naxyi;
                    }
                    fromMessage = response->messages_.back()->id_ + 1;
                    messages.insert(messages.end(), std::make_move_iterator(response->messages_.begin()), std::make_move_iterator(response->messages_.end()));
                }
            }
            {
                td::td_api::array<td::td_api::int53> readMessages;
                for (auto& msg: messages | ranges::view::reverse) {
                    readMessages.push_back(msg->id_);
                    int64_t senderId{};
                    td::td_api::downcast_call(*msg->sender_id_,
                                              aui::lambda_overloaded{
                                                  [&](td::td_api::messageSenderUser& user) { senderId = user.user_id_; },
                                                  [](td::td_api::messageSenderChat& chat) {},
                                              });
                    AString senderName;
                    if (senderId == mTelegram->myId()) {
                        senderName = "You (Kuni)";
                    } else {
                        auto sender =
                            co_await mTelegram->sendQueryWithResult(TelegramClient::toPtr(td::td_api::getUser(senderId)));
                        senderName = sender->first_name_ + " " + sender->last_name_;
                    }
                    result += "<message id=\"{}\" sender=\"{}\">\n"_format(msg->id_, senderName);
                    result += to_string(msg->content_);
                    result += "</message>\n\n";
                }
                result += R"(
<instructions>
Pay close attention to these messages. Acquire context from them and respond accordingly.

Real people, whom you are interacting via Telegram with, will not see your "plain text" responses. They'll see
#send_telegram_message only. Be careful distinguishing between chat with "user", and real people from Telegram.

You do not need to greet each time you receive a new message.
</instructions>
)";

                mTelegram->sendQuery(
                    TelegramClient::toPtr(td::td_api::viewMessages(chatId, std::move(readMessages), nullptr, false)));
            }

            naxyi:
            tools = OpenAITools{
                {
                    .name = "send_telegram_message",
                    .description = "Sends a message to the \"{}\" chat"_format(chat->title_),
                    .parameters =
                        {
                            .properties =
                                {
                                    {"message", {.type = "string", .description = "Contents of the message"}},
                                },
                            .required = {"message"},
                        },
                    .handler = [this, chatId, chatCloseMarker](OpenAITools::Ctx ctx) -> AFuture<AString> {
                        const auto& object = ctx.args.asObjectOpt().valueOrException("object expected");
                        auto message = object["message"].asStringOpt().valueOrException("`message` string expected");
                        co_await telegramPostMessage(chatId, message);
                        co_return "Message sent successfully.";
                    },
                },
            };

            co_return result;
        }
    };
} // namespace


AUI_ENTRY {
    using namespace std::chrono_literals;
    auto app = _new<App>();

    _new<AThread>([] {
        std::cin.get();
        gEventLoop.stop();
    })->start();

    AAsyncHolder async;
    async << [](_<App> app) -> AFuture<> {
        co_await app->telegram()->waitForConnection();
        // app->actProactively(); // for tests
    }(app);

    IEventLoop::Handle h(&gEventLoop);
    gEventLoop.loop();

    ALogger::info(LOG_TAG) << "Bot is shutting down. Please give some time to dump remaining context";
    auto d = app->dairyDumpMessages();
    while (!d.hasResult()) {
        AThread::processMessages();
    }

    return 0;
}
