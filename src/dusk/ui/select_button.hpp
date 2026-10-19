#pragma once

#include "component.hpp"
#include "ui.hpp"

#include <functional>
#include <utility>

namespace dusk::ui {

using SelectButtonCallback = std::function<void()>;

class SelectButton : public FluentComponent<SelectButton> {
public:
    struct Props {
        Rml::String key;
        Rml::String value;
        Rml::String icon;
        bool modified = false;
        bool submit = true;
    };

    SelectButton(Rml::Element* parent, Props props);

    virtual bool modified() const;
    void set_modified(bool value);
    void set_key(const Rml::String& key);
    void set_value_label(const Rml::String& value);
    SelectButton& on_pressed(SelectButtonCallback callback);

protected:
    void update_props(Props props);
    virtual bool handle_nav_command(NavCommand cmd);

    Props mProps;
    Rml::Element* mKeyElem = nullptr;
    Rml::Element* mIconElem = nullptr;
    Rml::Element* mValueElem = nullptr;
};

class BaseControlledSelectButton : public SelectButton {
public:
    BaseControlledSelectButton(Rml::Element* parent, Props props)
        : SelectButton(parent, std::move(props)) {}

    void update() override;

protected:
    virtual Rml::String format_value() = 0;
    // A hidden control is removed from layout and treated as disabled, so
    // keyboard/controller navigation skips it.
    virtual bool hidden() const { return false; }
};

class ControlledSelectButton : public BaseControlledSelectButton {
public:
    struct Props {
        Rml::String key;
        std::function<Rml::String()> getValue;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
        std::function<bool()> isHidden;
        bool submit = true;
    };

    ControlledSelectButton(Rml::Element* parent, Props props)
        : BaseControlledSelectButton(parent,
              {
                  .key = std::move(props.key),
                  .submit = props.submit,
              }),
          mGetValue(std::move(props.getValue)), mIsDisabled(std::move(props.isDisabled)),
          mIsModified(std::move(props.isModified)), mIsHidden(std::move(props.isHidden)) {}

    bool modified() const override;
    bool disabled() const override;

protected:
    Rml::String format_value() override;
    bool hidden() const override;

    std::function<Rml::String()> mGetValue;
    std::function<bool()> mIsDisabled;
    std::function<bool()> mIsModified;
    std::function<bool()> mIsHidden;
};

}  // namespace dusk::ui
