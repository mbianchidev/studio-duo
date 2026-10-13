#include "StartupProgressComponent.h"

#include "ReentrancySafeTimer.h"
#include "StudioTheme.h"
#include "platform/Architecture.h"

#include <cmath>

namespace studio
{
StartupProgressComponent::StartupProgressComponent()
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setTitle("Studio Duo startup");
    for (auto* label : { &title, &build, &current, &notice })
    {
        addAndMakeVisible(*label);
        label->setJustificationType(juce::Justification::centred);
    }
    title.setText("STUDIO DUO", juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(30.0f, juce::Font::bold)));
    build.setText("Version " + juce::String(STUDIO_DUO_VERSION) + " / "
        + juce::SystemStats::getOperatingSystemName()
        + " / " + currentArchitecture()
#if defined(STUDIO_DUO_BUILD_REVISION)
        + " / build " + juce::String(STUDIO_DUO_BUILD_REVISION)
#endif
        , juce::dontSendNotification);
    build.setFont(juce::Font(juce::FontOptions(12.0f)));
    current.setFont(juce::Font(juce::FontOptions(16.0f)));
    current.setText("Welcome. Preparing your local workspace.", juce::dontSendNotification);
    progress.setTitle("Required startup progress");
    addAndMakeVisible(progress);
    history.setMultiLine(true);
    history.setReadOnly(true);
    history.setScrollbarsShown(true);
    history.setCaretVisible(false);
    history.setTitle("Timestamped startup step history");
    history.setDescription("Use arrow keys and Page Up or Page Down to read completed and failed startup steps.");
    addChildComponent(history);
    historyButton.setClickingTogglesState(true);
    historyButton.onClick = [this]
    {
        const auto expanded = historyButton.getToggleState();
        history.setVisible(expanded);
        historyButton.setButtonText(expanded ? "Hide step history" : "Show step history");
        resized();
    };
    for (auto* button : { &historyButton, &retry, &openIo, &withoutAudio, &safeStart,
                          &resume, &cancelScan, &exportReport, &close })
    {
        addAndMakeVisible(*button);
        button->setWantsKeyboardFocus(true);
        button->setTitle(button->getButtonText());
    }
    retry.onClick = [this] { juce::NullCheckedInvocation::invoke(onRetry); };
    openIo.onClick = [this] { juce::NullCheckedInvocation::invoke(onOpenIo); };
    withoutAudio.onClick = [this] { juce::NullCheckedInvocation::invoke(onWithoutAudio); };
    safeStart.onClick = [this] { juce::NullCheckedInvocation::invoke(onSafeStart); };
    resume.onClick = [this] { juce::NullCheckedInvocation::invoke(onResume); };
    cancelScan.onClick = [this] { juce::NullCheckedInvocation::invoke(onCancelScan); };
    exportReport.onClick = [this] { juce::NullCheckedInvocation::invoke(onExport); };
    close.onClick = [this] { juce::NullCheckedInvocation::invoke(onClose); };
    for (auto* button : { &retry, &openIo, &withoutAudio, &safeStart, &resume, &cancelScan, &close })
        button->setVisible(false);
}

void StartupProgressComponent::setSnapshot(
    const StartupCoordinator& startup, bool workspaceAvailable)
{
    progress.setValue(startup.progress());
    const auto readiness = startup.readiness();
    canClose = workspaceAvailable
        && (readiness == StartupReadiness::ready || readiness == StartupReadiness::degraded);
    juce::StringArray running;
    juce::String summary;
    auto failure = false;
    for (const auto& item : startup.results())
    {
        if (item.state == StartupState::running)
            running.add(item.task.name);
        if (item.task.required && (item.state == StartupState::failed || item.state == StartupState::timedOut))
        {
            failure = true;
            if (summary.isEmpty())
                summary = item.task.name + ": " + item.detail;
        }
    }
    if (summary.isEmpty())
    {
        if (canClose)
            summary = readiness == StartupReadiness::ready
                ? "Ready. Required services are available."
                : "Workspace available in degraded mode. Review step history.";
        else
            summary = running.isEmpty() ? "Waiting for startup prerequisites."
                                       : "Starting: " + running.joinIntoString(", ");
    }
    current.setText(summary, juce::dontSendNotification);
    if (painted && summary != lastAnnouncement)
    {
        lastAnnouncement = summary;
        juce::AccessibilityHandler::postAnnouncement(summary,
            failure ? juce::AccessibilityHandler::AnnouncementPriority::high
                    : juce::AccessibilityHandler::AnnouncementPriority::low);
    }
    juce::String historyText;
    for (const auto& entry : startup.history())
    {
        historyText << entry.timestamp.formatted("%H:%M:%S") << "  "
                    << startupTaskId(entry.id) << "  " << startupStateName(entry.state)
                    << "  (" << juce::String(entry.durationMs, 1) << " ms)";
        if (entry.detail.isNotEmpty())
            historyText << "\n" << entry.detail;
        historyText << "\n\n";
    }
    if (history.getText() != historyText)
    {
        const auto readingHistory = history.hasKeyboardFocus(true);
        history.setText(historyText, false);
        if (!readingHistory)
            history.moveCaretToEnd();
    }
    const auto* recovery = startup.result(StartupTaskId::recovery);
    const auto recoveryPending = recovery != nullptr
        && recovery->issue == StartupIssue::recoveryRequired
        && !startupStateAllowsDependents(recovery->state);
    const auto* audio = startup.result(StartupTaskId::audio);
    const auto audioFailed = audio != nullptr && startupStateIsTerminal(audio->state)
        && !startupStateAllowsDependents(audio->state);
    retry.setVisible(audioFailed && !recoveryPending);
    retry.setEnabled(audio != nullptr && audio->attempt < audio->task.maximumAttempts);
    openIo.setVisible(audioFailed && workspaceAvailable && !recoveryPending);
    withoutAudio.setVisible(audioFailed && !recoveryPending);
    safeStart.setVisible(recoveryPending || (failure && !audioFailed));
    resume.setVisible(recoveryPending);
    const auto* scan = startup.result(StartupTaskId::pluginRescan);
    cancelScan.setVisible(scan != nullptr && scan->state == StartupState::running);
    close.setVisible(canClose);
    if (failure && !historyButton.getToggleState())
    {
        historyButton.setToggleState(true, juce::dontSendNotification);
        historyButton.setButtonText("Hide step history");
        history.setVisible(true);
    }
    resized();
}

void StartupProgressComponent::setNotice(const juce::String& message)
{
    notice.setText(message, juce::dontSendNotification);
    juce::AccessibilityHandler::postAnnouncement(message,
        juce::AccessibilityHandler::AnnouncementPriority::low);
}

bool StartupProgressComponent::hasPainted() const noexcept { return painted; }

void StartupProgressComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    graphics.setColour(juce::Colour(StudioColours::orange));
    graphics.fillRoundedRectangle(
        juce::Rectangle<float>(static_cast<float>(getWidth()) * 0.5f - 24.0f, 28.0f, 48.0f, 4.0f), 2.0f);
    if (!painted)
    {
        painted = true;
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<StartupProgressComponent>(this)]
        {
            if (safe != nullptr)
                juce::NullCheckedInvocation::invoke(safe->onFirstPaint);
        });
    }
}

void StartupProgressComponent::resized()
{
    auto area = getLocalBounds().reduced(32);
    area = area.withSizeKeepingCentre(juce::jmin(840, area.getWidth()), area.getHeight());
    title.setBounds(area.removeFromTop(62));
    build.setBounds(area.removeFromTop(26));
    current.setBounds(area.removeFromTop(74));
    progress.setBounds(area.removeFromTop(26));
    area.removeFromTop(12);
    historyButton.setBounds(area.removeFromTop(30).withSizeKeepingCentre(180, 30));
    area.removeFromTop(12);
    auto actions = area.removeFromBottom(88);
    notice.setBounds(area.removeFromBottom(42));
    history.setBounds(area);
    auto x = actions.getX();
    auto y = actions.getY();
    for (auto* button : { &retry, &openIo, &withoutAudio, &safeStart, &resume,
                          &cancelScan, &exportReport, &close })
    {
        if (!button->isVisible())
            continue;
        const auto width = button == &retry || button == &openIo ? 92 : 176;
        if (x + width > actions.getRight())
        {
            x = actions.getX();
            y += 40;
        }
        button->setBounds(x, y, width, 32);
        x += width + 8;
    }
}

bool StartupProgressComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && canClose)
    {
        juce::NullCheckedInvocation::invoke(onClose);
        return true;
    }
    return false;
}

void StartupProgressComponent::Progress::setValue(double next)
{
    if (juce::approximatelyEqual(value, next))
        return;
    value = next;
    repaint();
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

void StartupProgressComponent::Progress::paint(juce::Graphics& graphics)
{
    auto area = getLocalBounds();
    const auto percentage = area.removeFromRight(52);
    const auto bounds = area.toFloat();
    graphics.setColour(juce::Colour(StudioColours::raised));
    graphics.fillRoundedRectangle(bounds, 4.0f);
    graphics.setColour(juce::Colour(StudioColours::orange));
    graphics.fillRoundedRectangle(bounds.withWidth(bounds.getWidth() * static_cast<float>(value)), 4.0f);
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.drawText(juce::String(static_cast<int>(std::round(value * 100.0))) + "%",
                      percentage, juce::Justification::centredRight);
}

std::unique_ptr<juce::AccessibilityHandler>
StartupProgressComponent::Progress::createAccessibilityHandler()
{
    class Value final : public juce::AccessibilityRangedNumericValueInterface
    {
    public:
        explicit Value(Progress& bar) : owner(bar) {}
        bool isReadOnly() const override { return true; }
        double getCurrentValue() const override { return owner.value; }
        void setValue(double) override { jassertfalse; }
        AccessibleValueRange getRange() const override { return { { 0.0, 1.0 }, 0.01 }; }
    private:
        Progress& owner;
    };
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::progressBar, juce::AccessibilityActions(),
        juce::AccessibilityHandler::Interfaces(std::make_unique<Value>(*this)));
}
}
