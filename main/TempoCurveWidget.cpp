/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Performance Precision
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "TempoCurveWidget.h"

#include "svgui/layer/ColourDatabase.h"

#include <QPainter>

using namespace std;
using namespace sv;

TempoCurveWidget::TempoCurveWidget(QWidget *parent) :
    QFrame(parent),
    m_colourCounter(0),
    m_highlightedPosition(-1.0),
    m_audioModelDisplayBegin(0),
    m_audioModelDisplayEnd(0)
{
}

TempoCurveWidget::~TempoCurveWidget()
{
}

void
TempoCurveWidget::setCurveForAudio(sv::ModelId audioModel, sv::ModelId tempoModel)
{
    m_curves[audioModel] = tempoModel;

    ColourDatabase *cdb = ColourDatabase::getInstance();
    QColor colour = Qt::black;

    while (colour == Qt::black || colour == Qt::white) {
        colour = cdb->getColour(m_colourCounter % cdb->getColourCount());
        ++m_colourCounter;
    }
    
    m_colours[audioModel] = colour;
}

void
TempoCurveWidget::unsetCurveForAudio(sv::ModelId audioModel)
{
    m_curves.erase(audioModel);
    m_colours.erase(audioModel);
}

void
TempoCurveWidget::setHighlightedPosition(QString label)
{
    SVDEBUG << "TempoCurveWidget::setHighlightedPosition("
            << label << ")" << endl;

    bool ok = false;
    double bar = labelToBarAndFraction(label, &ok);
    if (!ok) {
        SVDEBUG << "TempoCurveWidget::setHighlightedPosition: unable to parse "
                << "label \"" << label << "\"" << endl;
        return;
    }

    m_highlightedPosition = bar;
    update();
}

void
TempoCurveWidget::setCurrentAudioModel(ModelId model)
{
    m_currentAudioModel = model;
    update();
}

void
TempoCurveWidget::setAudioModelDisplayedRange(sv_frame_t begin, sv_frame_t end)
{
    m_audioModelDisplayBegin = begin;
    m_audioModelDisplayEnd = end;
    update();
}

void
TempoCurveWidget::paintEvent(QPaintEvent *e)
{
    QFrame::paintEvent(e);

    {
        QPainter paint(this);
        paint.fillRect(rect(), Qt::white);
    }

    double barStart = frameToBarAndFraction(m_audioModelDisplayBegin,
                                            m_currentAudioModel);
    double barEnd = frameToBarAndFraction(m_audioModelDisplayEnd,
                                          m_currentAudioModel);
    if (barEnd <= 1.0) {
        return;
    }
    if (barStart < 1.0) {
        barStart = 1.0;
    }
    
    paintBarAndBeatLines(barStart, barEnd, 4.0);
    
    for (auto c: m_curves) {
        if (auto model = ModelById::getAs<SparseTimeValueModel>(c.second)) {
            paintCurve(model, m_colours[c.first], barStart, barEnd);
        }
    }

    if (m_highlightedPosition >= 0.0) {
        double x = barToX(m_highlightedPosition, barStart, barEnd);
        QPainter paint(this);
        QColor highlightColour("#59c4df");
        highlightColour.setAlpha(160);
        paint.setPen(Qt::NoPen);
        paint.setBrush(highlightColour);
        paint.drawRect(QRectF(x, 0.0, 10.0, height()));
    }
}

double
TempoCurveWidget::frameToBarAndFraction(sv_frame_t frame, ModelId audioModelId)
    const
{
    if (m_curves.find(audioModelId) == m_curves.end()) {
        return 0.0;
    }
    auto tempoModel = ModelById::getAs<SparseTimeValueModel>
        (m_curves.at(audioModelId));
    if (!tempoModel) {
        return 0.0;
    }
    Event event;
    if (!tempoModel->getNearestEventMatching(frame,
                                             [](Event) { return true; },
                                             EventSeries::Backward,
                                             event)) {
        return 0.0;
    }
    bool ok = false;
    double bar = labelToBarAndFraction(event.getLabel(), &ok);
    if (!ok) {
        return 0.0;
    }
    return bar;
}

double
TempoCurveWidget::labelToBarAndFraction(QString label, bool *okp) const
{
    bool okv = false;
    bool &ok = (okp ? *okp : okv);
    
    ok = false;

    QStringList barAndFraction = label.split("+");
    if (barAndFraction.size() != 2) return 0.0;

    int bar = barAndFraction[0].toInt(&ok);
    if (!ok) return 0.0;

    QStringList numAndDenom = barAndFraction[1].split("/");
    if (numAndDenom.size() != 2) return 0.0;

    int num = numAndDenom[0].toInt(&ok);
    if (!ok) return 0.0;
    
    int denom = numAndDenom[1].toInt(&ok);
    if (!ok) return 0.0;

    ok = true;
    return double(bar) + double(num) / double(denom);
}

double
TempoCurveWidget::barToX(double bar, double barStart, double barEnd) const
{
    double margin = 16.0;
    double w = width() - margin;
    if (w < 0.0) w = 1.0;
    return margin + w * ((bar - barStart) / (barEnd - barStart));
}

void
TempoCurveWidget::paintBarAndBeatLines(double barStart, double barEnd,
                                       int beatsPerBar)
{
    QPainter paint(this);
    paint.setRenderHint(QPainter::Antialiasing, true);
    paint.setBrush(Qt::NoBrush);

    for (double bar = floor(barStart); bar <= barEnd; bar += 1.0) {

        double x = barToX(bar, barStart, barEnd);
        paint.setPen(Qt::black);
        paint.drawLine(x, 0, x, height());

        //!!! +font
        paint.drawText(x + 5, 5 + paint.fontMetrics().ascent(),
                       QString("%1").arg(int(bar)));
        
        for (int i = 1; i < beatsPerBar; ++i) {
            double barFrac = bar + double(i) / double(beatsPerBar);
            x = barToX(barFrac, barStart, barEnd);
            paint.setPen(Qt::gray);
            paint.drawLine(x, 0, x, height());
        }
    }
}

void
TempoCurveWidget::paintCurve(shared_ptr<SparseTimeValueModel> model,
                             QColor colour, double barStart, double barEnd)
{
    QPainter paint(this);
    paint.setRenderHint(QPainter::Antialiasing, true);
    paint.setBrush(Qt::NoBrush);
     
    EventVector points(model->getAllEvents()); //!!! for now...

    double w = width();
    double h = height();

    double maxValue = model->getValueMaximum();
    double minValue = model->getValueMinimum();
    if (maxValue <= minValue) maxValue = minValue + 1.0;

    double px = 0.0;
    double py = 0.0;
    bool first = true;

    QPen pointPen(colour, 4.0);
    pointPen.setCapStyle(Qt::RoundCap);
    QPen linePen(colour, 1.0);
    
    for (auto p : points) {
        
        bool ok = false;
        double bar = labelToBarAndFraction(p.getLabel(), &ok);
        
        if (!ok) {
            SVDEBUG << "TempoCurveWidget::paintCurve: Failed to parse bar and fraction \"" << p.getLabel() << "\"" << endl;
            continue;
        }
        // For now we show an arbitrary fixed bar range 0-5
        if (bar < barStart) {
            continue;
        }
        if (bar > barEnd) {
            break;
        }

        double x = barToX(bar, barStart, barEnd);
        double y = h - (h * ((p.getValue() - minValue) / (maxValue - minValue)));

        SVCERR << "frame = " << p.getFrame() << ", label = " << p.getLabel() << ", bar = " << bar
               << ", value = " << p.getValue() << ", minValue = " << minValue
               << ", maxValue = " << maxValue << ", w = " << w << ", h = "
               << h << ", x = " << x << ", y = " << y << endl;
        
        if (!first) {
            paint.setPen(linePen);
            paint.drawLine(px, py, x, y);
        }
            
        paint.setPen(pointPen);
        paint.drawPoint(x, y);

        px = x;
        py = y;
        first = false;
    }

}



