//========================================================================
//
// pdfinfo.cc
//
// Copyright 1998-2003 Glyph & Cog, LLC
// Copyright 2013 Igalia S.L.
//
//========================================================================

//========================================================================
//
// Modified under the Poppler project - http://poppler.freedesktop.org
//
// All changes made under the Poppler project to this file are licensed
// under GPL version 2 or later
//
// Copyright (C) 2006 Dom Lachowicz <cinamod@hotmail.com>
// Copyright (C) 2007-2010, 2012, 2016-2022, 2024-2026 Albert Astals Cid <aacid@kde.org>
// Copyright (C) 2010 Hib Eris <hib@hiberis.nl>
// Copyright (C) 2011 Vittal Aithal <vittal.aithal@cognidox.com>
// Copyright (C) 2012, 2013, 2016-2018, 2021 Adrian Johnson <ajohnson@redneon.com>
// Copyright (C) 2012 Fabio D'Urso <fabiodurso@hotmail.it>
// Copyright (C) 2013 Adrian Perez de Castro <aperez@igalia.com>
// Copyright (C) 2013 Suzuki Toshiya <mpsuzuki@hiroshima-u.ac.jp>
// Copyright (C) 2018 Klarälvdalens Datakonsult AB, a KDAB Group company, <info@kdab.com>. Work sponsored by the LiMux project of the city of Munich
// Copyright (C) 2018 Adam Reichold <adam.reichold@t-online.de>
// Copyright (C) 2018 Evangelos Rigas <erigas@rnd2.org>
// Copyright (C) 2019 Christian Persch <chpe@src.gnome.org>
// Copyright (C) 2019-2021 Oliver Sander <oliver.sander@tu-dresden.de>
// Copyright (C) 2019 Thomas Fischer <fischer@unix-ag.uni-kl.de>
// Copyright (C) 2024-2026 g10 Code GmbH, Author: Sune Stolborg Vuorela <sune@vuorela.dk>
// Copyright (C) 2025 Jonathan Hähne <jonathan.haehne@hotmail.com>
// Copyright (C) 2026 evil rabbit <evilrabbit@tutamail.com>
//
// To see a description of the changes please see the Changelog file that
// came with your tarball or type make ChangeLog if you are building from git
//
//========================================================================

#include "config.h"
#include <poppler-config.h>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <cmath>
#include <numbers>
#include <set>
#include "parseargs.h"
#include "goo/GooString.h"
#include "goo/gfile.h"
#include "goo/glibc.h"
#include "GlobalParams.h"
#include "Object.h"
#include "Stream.h"
#include "Array.h"
#include "Dict.h"
#include "XRef.h"
#include "Catalog.h"
#include "Page.h"
#include "PDFDoc.h"
#include "PDFDocFactory.h"
#include "CharTypes.h"
#include "UnicodeMap.h"
#include "UTF.h"
#include "Error.h"
#include "DateInfo.h"
#include "JSInfo.h"
#include "Win32Console.h"

static int firstPage = 1;
static int lastPage = 0;
static bool output_yaml = false;

static const ArgDesc argDesc[] = { { .arg = "-y", .kind = argFlag, .val = &output_yaml, .size = 0, .usage = "output as YAML" }, {} };

// sanitize output to prevent terminal escape injection and line spoofing
static bool isDangerousCtrl(unsigned char c)
{
    switch (c) {
    case 0x07: // BEL - audible bell, terminates OSC
    case 0x08: // BS  - erases previous character
    case 0x0a: // LF  - newline injection
    case 0x0b: // VT  - acts as linefeed
    case 0x0c: // FF  - acts as linefeed
    case 0x0d: // CR  - overwrites line start
    case 0x0e: // SO  - corrupts charset
    case 0x0f: // SI  - corrupts charset
    case 0x1b: // ESC - starts escape sequences
    case 0x7f: // DEL - may erase
        return true;
    default:
        return false;
    }
}

static void putSanitized(const char *s, int len, FILE *f)
{
    for (int i = 0; i < len; i++) {
        auto c = static_cast<unsigned char>(s[i]);
        fputc(isDangerousCtrl(c) ? '?' : c, f);
    }
}

// Like putSanitized, but also escapes characters that are significant inside
// a YAML double-quoted scalar ('"' and '\') so the resulting stream is always
// valid YAML.
static void putYamlEscaped(const char *s, int len, FILE *f)
{
    for (int i = 0; i < len; i++) {
        auto c = static_cast<unsigned char>(s[i]);
        if (isDangerousCtrl(c)) {
            fputc('?', f);
        } else if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else {
            fputc(c, f);
        }
    }
}

static void printYamlTextString(const std::string &s, const UnicodeMap *uMap)
{
    char buf[8];
    const std::vector<Unicode> u = TextStringToUCS4(s);
    for (const auto &c : u) {
        int n = uMap->mapUnicode(c, buf, sizeof(buf));
        putYamlEscaped(buf, n, stdout);
    }
}

static void printStdTextString(const std::string &s, const UnicodeMap *uMap)
{
    char buf[8];
    const std::vector<Unicode> u = TextStringToUCS4(s);
    for (const auto &c : u) {
        int n = uMap->mapUnicode(c, buf, sizeof(buf));
        putSanitized(buf, n, stdout);
    }
}

static void printInfoString(Dict *infoDict, const char *key, const char *text, const UnicodeMap *uMap)
{
    Object obj = infoDict->lookup(key);
    if (obj.isString()) {
        fputs(text, stdout);
        const std::string &s1 = obj.getString();
        printStdTextString(s1, uMap);
        fputc('\n', stdout);
    }
}

// YAML equivalent of printInfoString: emits `<indent><yamlKey>: "<value>"`
// with the value double-quote-escaped, or nothing if the key is absent.
static void printInfoStringYaml(Dict *infoDict, const char *key, const char *yamlKey, const UnicodeMap *uMap, unsigned indent = 0)
{
    Object obj = infoDict->lookup(key);
    if (obj.isString()) {
        for (unsigned i = 0; i < indent; i++) {
            fputs("  ", stdout);
        }
        printf("%s: \"", yamlKey);
        printYamlTextString(obj.getString(), uMap);
        printf("\"\n");
    }
}

static void printInfoDate(Dict *infoDict, const char *key, const char *text, const UnicodeMap *uMap)
{
    int year, mon, day, hour, min, sec, tz_hour, tz_minute;
    char tz;
    struct tm tmStruct;
    time_t time;
    char buf[256];

    Object obj = infoDict->lookup(key);
    if (obj.isString()) {
        fputs(text, stdout);
        const std::string &s = obj.getString();
        // TODO do something with the timezone info
        if (parseDateString(s, &year, &mon, &day, &hour, &min, &sec, &tz, &tz_hour, &tz_minute)) {
            tmStruct.tm_year = year - 1900;
            tmStruct.tm_mon = mon - 1;
            tmStruct.tm_mday = day;
            tmStruct.tm_hour = hour;
            tmStruct.tm_min = min;
            tmStruct.tm_sec = sec;
            tmStruct.tm_wday = -1;
            tmStruct.tm_yday = -1;
            tmStruct.tm_isdst = -1;
            // compute the tm_wday and tm_yday fields
            time = timegm(&tmStruct);
            if (time != static_cast<time_t>(-1)) {
                int offset = (tz_hour * 60 + tz_minute) * 60;
                if (tz == '-') {
                    offset *= -1;
                }
                time -= offset;
                localtime_r(&time, &tmStruct);
                strftime(buf, sizeof(buf), "%c %Z", &tmStruct);
                fputs(buf, stdout);
            } else {
                printStdTextString(s, uMap);
            }
        } else {
            printStdTextString(s, uMap);
        }
        fputc('\n', stdout);
    }
}

static void printPdfSubtype(PDFDoc *doc, const UnicodeMap *uMap)
{
    const Object info = doc->getDocInfo();
    if (info.isDict()) {
        const PDFSubtype pdftype = doc->getPDFSubtype();

        if ((pdftype == subtypeNull) | (pdftype == subtypeNone)) {
            return;
        }

        std::unique_ptr<GooString> part;
        std::unique_ptr<GooString> abbr;
        std::unique_ptr<GooString> standard;
        std::unique_ptr<GooString> typeExp;
        std::unique_ptr<GooString> confExp;

        // Form title from PDFSubtype
        switch (pdftype) {
        case subtypePDFA:
            printInfoString(info.getDict(), "GTS_PDFA1Version", "PDF subtype:    ", uMap);
            typeExp = std::make_unique<GooString>("ISO 19005 - Electronic document file format for long-term preservation (PDF/A)");
            standard = std::make_unique<GooString>("ISO 19005");
            abbr = std::make_unique<GooString>("PDF/A");
            break;
        case subtypePDFE:
            printInfoString(info.getDict(), "GTS_PDFEVersion", "PDF subtype:    ", uMap);
            typeExp = std::make_unique<GooString>("ISO 24517 - Engineering document format using PDF (PDF/E)");
            standard = std::make_unique<GooString>("ISO 24517");
            abbr = std::make_unique<GooString>("PDF/E");
            break;
        case subtypePDFUA:
            printInfoString(info.getDict(), "GTS_PDFUAVersion", "PDF subtype:    ", uMap);
            typeExp = std::make_unique<GooString>("ISO 14289 - Electronic document file format enhancement for accessibility (PDF/UA)");
            standard = std::make_unique<GooString>("ISO 14289");
            abbr = std::make_unique<GooString>("PDF/UA");
            break;
        case subtypePDFVT:
            printInfoString(info.getDict(), "GTS_PDFVTVersion", "PDF subtype:    ", uMap);
            typeExp = std::make_unique<GooString>("ISO 16612 - Electronic document file format for variable data exchange (PDF/VT)");
            standard = std::make_unique<GooString>("ISO 16612");
            abbr = std::make_unique<GooString>("PDF/VT");
            break;
        case subtypePDFX:
            printInfoString(info.getDict(), "GTS_PDFXVersion", "PDF subtype:    ", uMap);
            typeExp = std::make_unique<GooString>("ISO 15930 - Electronic document file format for prepress digital data exchange (PDF/X)");
            standard = std::make_unique<GooString>("ISO 15930");
            abbr = std::make_unique<GooString>("PDF/X");
            break;
        case subtypeNone:
        case subtypeNull:
        default:
            return;
        }

        // Form the abbreviation from PDFSubtypePart and PDFSubtype
        const PDFSubtypePart subpart = doc->getPDFSubtypePart();
        switch (pdftype) {
        case subtypePDFX:
            switch (subpart) {
            case subtypePart1:
                abbr->append("-1:2001");
                break;
            case subtypePart2:
                abbr->append("-2");
                break;
            case subtypePart3:
                abbr->append("-3:2002");
                break;
            case subtypePart4:
                abbr->append("-1:2003");
                break;
            case subtypePart5:
                abbr->append("-2");
                break;
            case subtypePart6:
                abbr->append("-3:2003");
                break;
            case subtypePart7:
                abbr->append("-4");
                break;
            case subtypePart8:
                abbr->append("-5");
                break;
            default:
                break;
            }
            break;
        case subtypeNone:
        case subtypeNull:
            break;
        default:
            GooString::appendf(abbr->toNonConstStr(), "-{0:d}", subpart);
            break;
        }

        // Form standard from PDFSubtypePart
        switch (subpart) {
        case subtypePartNone:
        case subtypePartNull:
            break;
        default:
            GooString::appendf(standard->toNonConstStr(), "-{0:d}", subpart);
            break;
        }

        // Form the subtitle from PDFSubtypePart and PDFSubtype
        switch (pdftype) {
        case subtypePDFA:
            switch (subpart) {
            case subtypePart1:
                part = std::make_unique<GooString>("Use of PDF 1.4");
                break;
            case subtypePart2:
                part = std::make_unique<GooString>("Use of ISO 32000-1");
                break;
            case subtypePart3:
                part = std::make_unique<GooString>("Use of ISO 32000-1 with support for embedded files");
                break;
            default:
                break;
            }
            break;
        case subtypePDFE:
            switch (subpart) {
            case subtypePart1:
                part = std::make_unique<GooString>("Use of PDF 1.6");
                break;
            default:
                break;
            }
            break;
        case subtypePDFUA:
            switch (subpart) {
            case subtypePart1:
                part = std::make_unique<GooString>("Use of ISO 32000-1");
                break;
            case subtypePart2:
                part = std::make_unique<GooString>("Use of ISO 32000-2");
                break;
            case subtypePart3:
                part = std::make_unique<GooString>("Use of ISO 32000-1 with support for embedded files");
                break;
            default:
                break;
            }
            break;
        case subtypePDFVT:
            switch (subpart) {
            case subtypePart1:
                part = std::make_unique<GooString>("Using PPML 2.1 and PDF 1.4");
                break;
            case subtypePart2:
                part = std::make_unique<GooString>("Using PDF/X-4 and PDF/X-5 (PDF/VT-1 and PDF/VT-2)");
                break;
            case subtypePart3:
                part = std::make_unique<GooString>("Using PDF/X-6 (PDF/VT-3)");
                break;
            default:
                break;
            }
            break;
        case subtypePDFX:
            switch (subpart) {
            case subtypePart1:
                part = std::make_unique<GooString>("Complete exchange using CMYK data (PDF/X-1 and PDF/X-1a)");
                break;
            case subtypePart3:
                part = std::make_unique<GooString>("Complete exchange suitable for colour-managed workflows (PDF/X-3)");
                break;
            case subtypePart4:
                part = std::make_unique<GooString>("Complete exchange of CMYK and spot colour printing data using PDF 1.4 (PDF/X-1a)");
                break;
            case subtypePart5:
                part = std::make_unique<GooString>("Partial exchange of printing data using PDF 1.4 (PDF/X-2) [Withdrawn]");
                break;
            case subtypePart6:
                part = std::make_unique<GooString>("Complete exchange of printing data suitable for colour-managed workflows using PDF 1.4 (PDF/X-3)");
                break;
            case subtypePart7:
                part = std::make_unique<GooString>("Complete exchange of printing data (PDF/X-4) and partial exchange of printing data with external profile reference (PDF/X-4p) using PDF 1.6");
                break;
            case subtypePart8:
                part = std::make_unique<GooString>("Partial exchange of printing data using PDF 1.6 (PDF/X-5)");
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }

        // Form Conformance explanation from PDFSubtypeConformance
        switch (doc->getPDFSubtypeConformance()) {
        case subtypeConfA:
            confExp = std::make_unique<GooString>("Level A, Accessible");
            break;
        case subtypeConfB:
            confExp = std::make_unique<GooString>("Level B, Basic");
            break;
        case subtypeConfG:
            confExp = std::make_unique<GooString>("Level G, External graphical content");
            break;
        case subtypeConfN:
            confExp = std::make_unique<GooString>("Level N, External ICC profile");
            break;
        case subtypeConfP:
            confExp = std::make_unique<GooString>("Level P, Embedded ICC profile");
            break;
        case subtypeConfPG:
            confExp = std::make_unique<GooString>("Level PG, Embedded ICC profile and external graphical content");
            break;
        case subtypeConfU:
            confExp = std::make_unique<GooString>("Level U, Unicode support");
            break;
        case subtypeConfNone:
        case subtypeConfNull:
        default:
            confExp.reset();
            break;
        }

        printf("    Title:         %s\n", typeExp->c_str());
        printf("    Abbreviation:  %s\n", abbr->c_str());
        if (part) {
            printf("    Subtitle:      Part %d: %s\n", subpart, part->c_str());
        } else {
            printf("    Subtitle:      Part %d\n", subpart);
        }
        printf("    Standard:      %s-%d\n", typeExp->toStr().substr(0, 9).c_str(), subpart);
        if (confExp) {
            printf("    Conformance:   %s\n", confExp->c_str());
        }
    }
}

static void printInfo(PDFDoc *doc, const UnicodeMap *uMap, long long filesize, bool multiPage)
{
    double w, h, wISO, hISO, isoThreshold;
    int pg, i;
    int r;

    // print doc info
    Object info = doc->getDocInfo();
    if (info.isDict()) {
        printInfoString(info.getDict(), "Title", "Title:           ", uMap);
        printInfoString(info.getDict(), "Subject", "Subject:         ", uMap);
        printInfoString(info.getDict(), "Keywords", "Keywords:        ", uMap);
        printInfoString(info.getDict(), "Author", "Author:          ", uMap);
        printInfoString(info.getDict(), "Creator", "Creator:         ", uMap);
        printInfoString(info.getDict(), "Producer", "Producer:        ", uMap);
        printInfoDate(info.getDict(), "CreationDate", "CreationDate:    ", uMap);
        printInfoDate(info.getDict(), "ModDate", "ModDate:         ", uMap);
    }

    bool hasMetadata = false;
    std::unique_ptr<GooString> metadata = doc->readMetadata();
    if (metadata) {
        hasMetadata = true;
    }

    const std::set<std::string> docInfoStandardKeys { "Title", "Author", "Subject", "Keywords", "Creator", "Producer", "CreationDate", "ModDate", "Trapped" };

    bool hasCustom = false;
    if (info.isDict()) {
        Dict *dict = info.getDict();
        for (i = 0; i < dict->getLength(); i++) {
            std::string key(dict->getKey(i));
            if (!docInfoStandardKeys.contains(key)) {
                hasCustom = true;
                break;
            }
        }
    }

    // print metadata info
    printf("Custom Metadata: %s\n", hasCustom ? "yes" : "no");
    printf("Metadata Stream: %s\n", hasMetadata ? "yes" : "no");

    // print tagging info
    printf("Tagged:          %s\n", (doc->getCatalog()->getMarkInfo() & Catalog::markInfoMarked) ? "yes" : "no");
    printf("UserProperties:  %s\n", (doc->getCatalog()->getMarkInfo() & Catalog::markInfoUserProperties) ? "yes" : "no");
    printf("Suspects:        %s\n", (doc->getCatalog()->getMarkInfo() & Catalog::markInfoSuspects) ? "yes" : "no");

    // print form info
    switch (doc->getCatalog()->getFormType()) {
    case Catalog::NoForm:
        printf("Form:            none\n");
        break;
    case Catalog::AcroForm:
        printf("Form:            AcroForm\n");
        break;
    case Catalog::XfaForm:
        printf("Form:            XFA\n");
        break;
    }

    // print javascript info
    {
        JSInfo jsInfo(doc, firstPage - 1);
        jsInfo.scanJS(lastPage - firstPage + 1);
        printf("JavaScript:      %s\n", jsInfo.containsJS() ? "yes" : "no");
    }

    // print page count
    printf("Pages:           %d\n", doc->getNumPages());

    // print encryption info
    printf("Encrypted:       ");
    if (doc->isEncrypted()) {
        unsigned char *fileKey;
        CryptAlgorithm encAlgorithm;
        int keyLength;
        doc->getXRef()->getEncryptionParameters(&fileKey, &encAlgorithm, &keyLength);

        const char *encAlgorithmName = "unknown";
        switch (encAlgorithm) {
        case cryptRC4:
            encAlgorithmName = "RC4";
            break;
        case cryptAES:
            encAlgorithmName = "AES";
            break;
        case cryptAES256:
            encAlgorithmName = "AES-256";
            break;
        case cryptNone:
            break;
        }

        printf("yes (print:%s copy:%s change:%s addNotes:%s algorithm:%s)\n", doc->okToPrint(true) ? "yes" : "no", doc->okToCopy(true) ? "yes" : "no", doc->okToChange(true) ? "yes" : "no", doc->okToAddNotes(true) ? "yes" : "no",
               encAlgorithmName);
    } else {
        printf("no\n");
    }

    // print page size
    for (pg = firstPage; pg <= lastPage; ++pg) {
        w = doc->getPageCropWidth(pg);
        h = doc->getPageCropHeight(pg);
        if (multiPage) {
            printf("Page %4d size:  %g x %g pts", pg, w, h);
        } else {
            printf("Page size:       %g x %g pts", w, h);
        }
        if ((fabs(w - 612) < 1 && fabs(h - 792) < 1) || (fabs(w - 792) < 1 && fabs(h - 612) < 1)) {
            printf(" (letter)");
        } else {
            hISO = sqrt(std::numbers::sqrt2) * 7200 / 2.54;
            wISO = hISO / std::numbers::sqrt2;
            isoThreshold = hISO * 0.003; ///< allow for 0.3% error when guessing conformance to ISO 216, A series
            for (i = 0; i <= 6; ++i) {
                if ((fabs(w - wISO) < isoThreshold && fabs(h - hISO) < isoThreshold) || (fabs(w - hISO) < isoThreshold && fabs(h - wISO) < isoThreshold)) {
                    printf(" (A%d)", i);
                    break;
                }
                hISO = wISO;
                wISO /= std::numbers::sqrt2;
                isoThreshold /= std::numbers::sqrt2;
            }
        }
        printf("\n");
        r = doc->getPageRotate(pg);
        if (multiPage) {
            printf("Page %4d rot:   %d\n", pg, r);
        } else {
            printf("Page rot:        %d\n", r);
        }
    }

    // print file size
    printf("File size:       %lld bytes\n", filesize);

    // print linearization info
    printf("Optimized:       %s\n", doc->isLinearized() ? "yes" : "no");

    // print PDF version
    printf("PDF version:     %d.%d\n", doc->getPDFMajorVersion(), doc->getPDFMinorVersion());

    printPdfSubtype(doc, uMap);
}

// YAML equivalent of printInfo: emits the same information as a single YAML
// document instead of the plain-text report.
static void printInfoYaml(PDFDoc *doc, const UnicodeMap *uMap, long long filesize, bool multiPage)
{

    // doc info
    Object info = doc->getDocInfo();
    if (info.isDict()) {
        printInfoStringYaml(info.getDict(), "Title", "title", uMap);
        printInfoStringYaml(info.getDict(), "Author", "author", uMap);
    }

    // print file size
    printf("File size:       %lld bytes\n", filesize);

    // print linearization info
    printf("Optimized:       %s\n", doc->isLinearized() ? "true" : "false");

    // print PDF version
    printf("PDF version:     %d.%d\n", doc->getPDFMajorVersion(), doc->getPDFMinorVersion());

    printPdfSubtype(doc, uMap);
}

int main(int argc, char *argv[])
{
    std::unique_ptr<PDFDoc> doc;
    GooString *fileName;
    std::optional<GooString> ownerPW, userPW;
    const UnicodeMap *uMap;
    FILE *f;
    bool ok;
    int exitCode;
    bool multiPage;

    exitCode = 99;

    // parse args
    Win32Console win32console(&argc, &argv);
    ok = parseArgs(argDesc, &argc, argv);
    if (!ok || argc != 2) {
        fprintf(stderr, "pdfinfo version %s\n", PACKAGE_VERSION);
        fprintf(stderr, "%s\n", popplerCopyright);
        fprintf(stderr, "%s\n", xpdfCopyright);
        printUsage("pdfinfo", "<PDF-file>", argDesc);
        goto err0;
    }

    // read config file
    globalParams = std::make_unique<GlobalParams>();

    fileName = new GooString(argv[1]);

    // get mapping to output encoding
    if (!(uMap = globalParams->getTextEncoding())) {
        error(errCommandLine, -1, "Couldn't get text encoding");
        delete fileName;
        goto err1;
    }

    if (fileName->compare("-") == 0) {
        delete fileName;
        fileName = new GooString("fd://0");
    }

    doc = PDFDocFactory().createPDFDoc(*fileName, ownerPW, userPW);

    if (!doc->isOk()) {
        exitCode = 1;
        goto err2;
    }

    // get page range
    if (firstPage < 1) {
        firstPage = 1;
    }
    multiPage = lastPage != 0;
    if (lastPage < 1 || lastPage > doc->getNumPages()) {
        lastPage = doc->getNumPages();
    }
    if (lastPage < firstPage) {
        error(errCommandLine, -1, "Wrong page range given: the first page ({0:d}) can not be after the last page ({1:d}).", firstPage, lastPage);
        goto err2;
    }

    {
        // print info
        long long filesize = 0;

        f = fopen(fileName->c_str(), "rb");
        if (f) {
            Gfseek(f, 0, SEEK_END);
            filesize = Gftell(f);
            fclose(f);
        }

        if (!multiPage) {
            lastPage = 1;
        }

        if (output_yaml) {
            printInfoYaml(doc.get(), uMap, filesize, multiPage);
        } else {
            printInfo(doc.get(), uMap, filesize, multiPage);
        }
    }
    exitCode = 0;

    // clean up
err2:
    delete fileName;
err1:
err0:

    return exitCode;
}
