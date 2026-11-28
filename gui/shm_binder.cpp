#include "shm_binder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QPair>
#include <QVector>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QWidget>

#include <cmath>

// Each Add* pulls its own value out of the header the moment the control exists, so a control is
// never showing something the header does not say. Relying on a single Reload() at the end of the
// panel worked only for as long as nobody added a control after it, which is the kind of ordering
// dependency that fails quietly and much later.
ShmBinder::ShmBinder(ShmHeader* header, QWidget* parent)
    : QObject(parent), _hdr(header), _parent(parent) {}

void ShmBinder::Write(Field field, uint32_t raw, Latch latch) {
    if (!_hdr || _reloading) return;
    (_hdr->*field).store(raw);
    _hdr->controlSeq.fetch_add(1);
    if (latch == AtCreate) _hdr->tuningSeq.fetch_add(1);
}

// ---------- SliderEdit ----------

SliderEdit::SliderEdit(QWidget* parent, int lo, int hi, int step, int value)
    : QWidget(parent), _isFloat(false), _loInt(lo), _hiInt(hi), _stepInt(step) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    _slider = new QSlider(Qt::Horizontal, this);
    _slider->setRange(0, kSliderSteps);
    _slider->setValue(valueToSliderPos(double(value)));
    lay->addWidget(_slider, 1);

    _edit = new QLineEdit(this);
    _edit->setFixedWidth(60);
    _edit->setText(QString::number(value));
    lay->addWidget(_edit, 0);

    connect(_slider, &QSlider::valueChanged, this, [this](int pos) {
        syncEditFromSlider();
        if (_isFloat) {
            emit doubleChanged(sliderPosToValue(pos));
        } else {
            emit intChanged(int(std::lround(sliderPosToValue(pos))));
        }
    });

    // editingFinished fires on Enter as well as on focus loss. A separate returnPressed handler
    // committed the same value twice, bumping controlSeq twice and emitting a second write.
    connect(_edit, &QLineEdit::editingFinished, this, [this]() {
        syncSliderFromEdit();
    });
}

SliderEdit::SliderEdit(QWidget* parent, double lo, double hi, double step, double value)
    : QWidget(parent), _isFloat(true), _loFloat(lo), _hiFloat(hi), _stepFloat(step) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    _slider = new QSlider(Qt::Horizontal, this);
    _slider->setRange(0, kSliderSteps);
    _slider->setValue(valueToSliderPos(value));
    lay->addWidget(_slider, 1);

    _edit = new QLineEdit(this);
    _edit->setFixedWidth(60);
    updateEditText();
    lay->addWidget(_edit, 0);

    connect(_slider, &QSlider::valueChanged, this, [this](int pos) {
        syncEditFromSlider();
        emit doubleChanged(sliderPosToValue(pos));
    });

    // editingFinished fires on Enter as well as on focus loss. A separate returnPressed handler
    // committed the same value twice, bumping controlSeq twice and emitting a second write.
    connect(_edit, &QLineEdit::editingFinished, this, [this]() {
        syncSliderFromEdit();
    });
}

int SliderEdit::valueToSliderPos(double v) const {
    if (_isFloat) {
        const double span = _hiFloat - _loFloat;
        if (span == 0.0) return 0;
        return int(std::lround((v - _loFloat) / span * kSliderSteps));
    } else {
        const double span = double(_hiInt - _loInt);
        if (span == 0.0) return 0;
        return int(std::lround((v - _loInt) / span * kSliderSteps));
    }
}

double SliderEdit::sliderPosToValue(int pos) const {
    if (_isFloat) {
        return _loFloat + double(pos) / kSliderSteps * (_hiFloat - _loFloat);
    } else {
        return _loInt + double(pos) / kSliderSteps * (_hiInt - _loInt);
    }
}

void SliderEdit::syncEditFromSlider() {
    // The slider's own valueChanged signal emits the change. Only mirror the number into the field;
    // and never do that while the user has the field focused, or their half-typed value disappears.
    if (isEditing()) return;
    QSignalBlocker block(_edit);
    updateEditText();
}

void SliderEdit::syncSliderFromEdit() {
    bool ok = false;
    double v = 0.0;
    if (_isFloat) {
        v = _edit->text().toDouble(&ok);
    } else {
        v = double(_edit->text().toInt(&ok));
    }
    if (!ok) {
        // Not a number: restore the last good value rather than leaving nonsense in the field.
        QSignalBlocker block(_edit);
        updateEditText();
        return;
    }

    const double parsed = v;
    if (_isFloat) {
        v = std::clamp(v, _loFloat, _hiFloat);
    } else {
        v = std::clamp(v, double(_loInt), double(_hiInt));
    }
    const int committed = _isFloat ? int(std::lround(v)) : int(v);

    // Block the slider, not `this`: QSlider::valueChanged is emitted by the slider object, so a
    // QSignalBlocker on the parent widget does nothing and the slider handler goes on to emit a
    // second, quantised value and rewrite the text with it. That is the "set it to 100 and it
    // bounces to 99 or 101" bug.
    {
        QSignalBlocker block(_slider);
        _slider->setValue(valueToSliderPos(v));
    }

    // Keep the number the user actually typed when it was in range. Reformatting from the slider
    // position loses precision on wide ranges (paper white 0.01..2000: typing 100 came back as
    // 100.01) because 10000 slider steps cannot represent every typed value exactly.
    if (parsed != v) {
        QSignalBlocker block(_edit);
        if (_isFloat) _edit->setText(QString::number(v, 'g', 6));
        else _edit->setText(QString::number(int(std::lround(v))));
    }

    if (_isFloat) emit doubleChanged(v);
    else emit intChanged(committed);
}

void SliderEdit::updateEditText() {
    double v = sliderPosToValue(_slider->value());
    if (_isFloat) {
        _edit->setText(QString::number(v, 'g', 6));
    } else {
        // lround, not int(): a value one floating-point ulp below the integer must not show as 99.
        _edit->setText(QString::number(int(std::lround(v))));
    }
}

int SliderEdit::intValue() const {
    return int(std::lround(sliderPosToValue(_slider->value())));
}

double SliderEdit::doubleValue() const {
    return sliderPosToValue(_slider->value());
}

void SliderEdit::setIntValue(int v) {
    if (isEditing()) return;  // a reload must not move the slider under an active edit
    const int pos = valueToSliderPos(double(v));
    {
        QSignalBlocker block(_slider);
        _slider->setValue(pos);
    }
    QSignalBlocker block(_edit);
    updateEditText();
    // The slider's valueChanged is blocked above, so emit the change here. Reload callers are
    // shielded by ShmBinder's _reloading guard; a direct caller still gets the value it asked for.
    emit intChanged(v);
}

void SliderEdit::setDoubleValue(double v) {
    if (isEditing()) return;
    const int pos = valueToSliderPos(v);
    {
        QSignalBlocker block(_slider);
        _slider->setValue(pos);
    }
    QSignalBlocker block(_edit);
    updateEditText();
    emit doubleChanged(v);
}

QCheckBox* ShmBinder::AddBool(QFormLayout* form, const QString& label, Field field, const QString& tip,
                              Latch latch, bool invert) {
    auto* w = new QCheckBox(label, _parent);
    w->setToolTip(FormatTip(tip));
    form->addRow(w);
    connect(w, &QCheckBox::toggled, this, [this, field, latch, invert](bool on) {
        const bool v = invert ? !on : on;
        Write(field, v ? 1u : 0u, latch);
    });
    _reloaders.push_back([this, w, field, invert] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const bool v = (_hdr->*field).load() != 0;
        w->setChecked(invert ? !v : v);
    });
    _reloaders.back()();
    return w;
}

QWidget* ShmBinder::AddInt(QFormLayout* form, const QString& label, Field field, int lo, int hi,
                           const QString& tip, Latch latch) {
    auto* w = new SliderEdit(_parent, lo, hi, 1, lo);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    auto* se = qobject_cast<SliderEdit*>(w);
    connect(se, &SliderEdit::intChanged, this,
            [this, field, latch](int v) { Write(field, uint32_t(v < 0 ? 0 : v), latch); });
    _reloaders.push_back([this, se, field] {
        if (!_hdr || se->isEditing()) return;
        QSignalBlocker block(se);
        se->setIntValue(int((_hdr->*field).load()));
    });
    _reloaders.back()();
    return w;
}

QWidget* ShmBinder::AddFloat(QFormLayout* form, const QString& label, Field field, double lo,
                             double hi, double step, const QString& tip, Latch latch) {
    auto* w = new SliderEdit(_parent, lo, hi, step, lo);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    auto* se = qobject_cast<SliderEdit*>(w);
    connect(se, &SliderEdit::doubleChanged, this,
            [this, field, latch](double v) { Write(field, FloatToBits(float(v)), latch); });
    _reloaders.push_back([this, se, field] {
        if (!_hdr || se->isEditing()) return;
        QSignalBlocker block(se);
        se->setDoubleValue(double(BitsToFloat((_hdr->*field).load())));
    });
    _reloaders.back()();
    return w;
}

QComboBox* ShmBinder::AddChoice(QFormLayout* form, const QString& label, Field field,
                                const QStringList& options, const QString& tip, Latch latch) {
    auto* w = new QComboBox(_parent);
    w->addItems(options);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, field, latch](int i) { Write(field, uint32_t(i < 0 ? 0 : i), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const int v = int((_hdr->*field).load());
        w->setCurrentIndex(v < w->count() ? v : 0);
    });
    _reloaders.back()();
    return w;
}

QComboBox* ShmBinder::AddChoiceValues(QFormLayout* form, const QString& label, Field field,
                                      const QVector<QPair<QString, uint32_t>>& options,
                                      const QString& tip, Latch latch) {
    auto* w = new QComboBox(_parent);
    for (const auto& option : options) w->addItem(option.first, option.second);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, w, field, latch](int i) {
                if (i >= 0 && i < w->count()) Write(field, w->itemData(i).toUInt(), latch);
            });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const uint32_t v = (_hdr->*field).load();
        for (int i = 0; i < w->count(); ++i) {
            if (w->itemData(i).toUInt() == v) { w->setCurrentIndex(i); return; }
        }
        w->setCurrentIndex(0);
    });
    _reloaders.back()();
    return w;
}

QWidget* ShmBinder::AddPercent(QFormLayout* form, const QString& label, Field field, int lo, int hi,
                               const QString& tip, Latch latch) {
    // Percent is stored as a fraction (0..1) but shown as 0..100 on the slider.
    auto* w = new SliderEdit(_parent, lo, hi, 5, lo);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    auto* se = qobject_cast<SliderEdit*>(w);
    connect(se, &SliderEdit::intChanged, this,
            [this, field, latch](int v) { Write(field, FloatToBits(float(v) / 100.0f), latch); });
    _reloaders.push_back([this, se, field] {
        if (!_hdr || se->isEditing()) return;
        QSignalBlocker block(se);
        se->setIntValue(int(std::lround(double(BitsToFloat((_hdr->*field).load())) * 100.0)));
    });
    _reloaders.back()();
    return w;
}

void ShmBinder::Reload() {
    _reloading = true;
    for (auto& r : _reloaders) r();
    _reloading = false;
}
