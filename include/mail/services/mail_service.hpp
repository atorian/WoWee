#pragma once

#include "mail/domain/mailbox.hpp"
#include "mail/services/mail.hpp"
#include "mail/services/ports.hpp"

#include <string>
#include <vector>

namespace wowee::core { class EventBus; }

namespace wowee::mail {

class MailService final : public IMailbox, public IMailboxFeed {
public:
    MailService(IMailGateway& gateway, IMailWorld& world, core::EventBus& events);

    // IMailbox: reading
    [[nodiscard]] bool isMailboxOpen() const override { return box_.isOpen(); }
    [[nodiscard]] const std::vector<MailMessage>& getMailInbox() const override { return box_.inbox(); }
    [[nodiscard]] int getSelectedMailIndex() const override { return box_.selected(); }
    [[nodiscard]] bool isMailComposeOpen() const override { return box_.isComposing(); }
    [[nodiscard]] bool hasNewMail() const override { return box_.hasNewMail(); }
    [[nodiscard]] const std::array<Enclosure, kMaxEnclosures>& getMailAttachments() const override { return box_.enclosures(); }
    [[nodiscard]] int getMailAttachmentCount() const override { return box_.enclosureCount(); }
    [[nodiscard]] std::string getMailSenderName(const MailMessage& mail) const override;
    std::string getMailDisplaySubject(const MailMessage& mail) override;

    // IMailbox: the panel
    void setSelectedMailIndex(int index) override { box_.select(index); }
    void openMailCompose() override { apply(box_.startComposing()); }
    void closeMailCompose() override { apply(box_.stopComposing()); }
    void setMailComposeShowing(bool showing) override { box_.composeShowing(showing); }

    // IMailbox: requests
    void openMailbox(uint64_t mailboxGuid) override;
    void closeMailbox() override { apply(box_.closed()); }
    void refreshMailList() override;
    void sendMail(const std::string& recipient, const std::string& subject,
                  const std::string& body, uint64_t money, uint64_t cod = 0) override;
    bool attachItemFromBackpack(int backpackIndex) override { return enclose(world_.backpackItem(backpackIndex)); }
    bool attachItemFromBag(int bagIndex, int slotIndex) override { return enclose(world_.bagItem(bagIndex, slotIndex)); }
    bool detachMailAttachment(int attachIndex) override;
    void clearMailAttachments() override { apply(box_.clearEnclosures()); }
    void mailTakeMoney(uint32_t mailId) override;
    void mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) override;
    void mailDelete(uint32_t mailId) override;
    void mailReturnToSender(uint32_t mailId) override;
    void mailMarkAsRead(uint32_t mailId) override;

    // IMailboxFeed
    void onMailboxShown(uint64_t mailboxGuid) override;
    void onInbox(std::vector<MailMessage> inbox) override;
    void onSendResult(uint32_t mailId, uint32_t action, uint32_t error) override;
    void onNewMail(bool waiting) override { apply(box_.newMail(waiting)); }
    void onPlayerNameResolved(uint64_t guid, const std::string& name) override { box_.senderNamed(guid, name); }
    void onItemInfo(uint32_t itemEntry) override { apply(box_.itemInfoArrived(itemEntry)); }
    void beginCharacter() override { box_.beginCharacter(); }

private:
    /// At a mailbox, in the world, with a connection.
    [[nodiscard]] bool ready() const;
    bool enclose(std::optional<Enclosure> item);
    void refuse(std::string reason, std::string logLine);
    void apply(const std::vector<MailChange>& changes);

    IMailGateway& gateway_;
    IMailWorld& world_;
    core::EventBus& events_;
    Mailbox box_;
};

} // namespace wowee::mail
