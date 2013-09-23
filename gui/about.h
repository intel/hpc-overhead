/*
Copyright (c) 2009-2013, Intel Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef ABOUT_H
#define ABOUT_H
#include <QDialog>
#include <QLabel>
#include <QDialogButtonBox>

class Ui_AboutDialog
{
public:
    QDialogButtonBox *aboutOK;
    QLabel *aboutLabel;

    void setupUi(QDialog *AboutDialog)
    {
        if (AboutDialog->objectName().isEmpty())
            AboutDialog->setObjectName(QStringLiteral("AboutDialog"));
        AboutDialog->resize(400, 300);
        AboutDialog->setWindowTitle("About overhead-gui");
        aboutOK = new QDialogButtonBox(QDialogButtonBox::Ok, Qt::Horizontal,AboutDialog);
        aboutOK->setObjectName(QStringLiteral("aboutOK"));
        aboutOK->setGeometry(QRect(170, 250, 60, 20));
        aboutLabel = new QLabel(AboutDialog);
        aboutLabel->setObjectName(QStringLiteral("aboutLabel"));
        aboutLabel->setGeometry(QRect(10, 10, 380, 150));
        QObject::connect(aboutOK, SIGNAL(accepted()), AboutDialog, SLOT(accept()));
        QMetaObject::connectSlotsByName(AboutDialog);

    } // setupUi

};

#endif
