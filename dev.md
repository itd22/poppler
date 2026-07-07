# program pdfinfo-yaml 

# added features

flag:  -y
output: yaml format

# Build instructions for pdfinfo-yaml

cmake -DENABLE_UTILS=ON -DENABLE_GTK_DOC=OFF -DBUILD_CPP_TESTS=OFF -DENABLE_BOOST=OFF  ..
cmake --build . --target pdfinfo-yaml

# usage example
poppler/build/utils/pdfinfo-yaml -y  ml_sanitized.pdf
title: "Machine Learning Platform Engineering"
author: "Benjamin Tan Wei Hao, Shanoop Padmanabhan, Varun Mallya"
size:       35772216
Optimized:       false
PDF version:     1.6
