#!/bin/bash

# 디렉토리 경로 (원하는 디렉토리 경로로 변경하세요)
TARGET_DIR=$1

# 확장자 지정 (예: .c와 .h 파일만 포함)
find "$TARGET_DIR" -name "*.c" -o -name "*.h" | xargs wc -l | tail -n 1

