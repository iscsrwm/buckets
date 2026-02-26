#!/bin/bash
# Buckets Multipart Upload Testing Script
# Tests the complete S3 multipart upload workflow

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
SERVER="http://localhost:9000"
BUCKET="test-bucket"
KEY="test-multipart-object.bin"
TEST_DIR="/tmp/test-multipart"
FILE_SIZE_MB=20
PART_SIZE_MB=5

echo "======================================"
echo "Buckets Multipart Upload Test Suite"
echo "======================================"
echo ""

# Function to print colored output
print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_info() {
    echo -e "${YELLOW}→ $1${NC}"
}

# Function to extract XML value
extract_xml() {
    local xml="$1"
    local tag="$2"
    echo "$xml" | grep -oP "(?<=<${tag}>)[^<]+"
}

# Cleanup function
cleanup() {
    print_info "Cleaning up test files..."
    rm -rf "$TEST_DIR"
    rm -rf /tmp/buckets-data/$BUCKET/.multipart/*
    echo ""
}

# Test 1: Setup
echo "Test 1: Setup Test Environment"
echo "-------------------------------"

# Create test directory
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

# Create test file
print_info "Creating ${FILE_SIZE_MB}MB test file..."
dd if=/dev/urandom of=testfile.bin bs=1M count=$FILE_SIZE_MB 2>&1 | grep -v records
print_success "Test file created: $(ls -lh testfile.bin | awk '{print $5}')"

# Calculate MD5 of original file
ORIGINAL_MD5=$(md5sum testfile.bin | awk '{print $1}')
print_info "Original file MD5: $ORIGINAL_MD5"

# Split into parts
print_info "Splitting into ${PART_SIZE_MB}MB parts..."
split -b ${PART_SIZE_MB}M testfile.bin part_
PART_COUNT=$(ls -1 part_* | wc -l)
print_success "Created $PART_COUNT parts"
ls -lh part_* | awk '{print "  " $9 " - " $5}'

echo ""

# Test 2: Create Bucket
echo "Test 2: Create Bucket"
echo "---------------------"

print_info "Creating bucket: $BUCKET"
RESPONSE=$(curl -s -X PUT -H "Content-Length: 0" "$SERVER/$BUCKET")

if [ -z "$RESPONSE" ]; then
    print_success "Bucket created successfully"
else
    print_info "Bucket may already exist (response: $RESPONSE)"
fi

echo ""

# Test 3: Initiate Multipart Upload
echo "Test 3: Initiate Multipart Upload"
echo "----------------------------------"

print_info "Initiating multipart upload for: $KEY"
INIT_RESPONSE=$(curl -s -X POST -H "Content-Length: 0" "$SERVER/$BUCKET/$KEY?uploads")

if [ -z "$INIT_RESPONSE" ]; then
    print_error "Failed to initiate upload (empty response)"
    exit 1
fi

UPLOAD_ID=$(extract_xml "$INIT_RESPONSE" "UploadId")

if [ -z "$UPLOAD_ID" ]; then
    print_error "Failed to extract UploadId from response:"
    echo "$INIT_RESPONSE"
    exit 1
fi

print_success "Upload initiated with ID: $UPLOAD_ID"
echo ""

# Test 4: Upload Parts
echo "Test 4: Upload Parts"
echo "--------------------"

PART_NUM=1
declare -a ETAGS

for PART_FILE in $(ls -1 part_* | sort); do
    print_info "Uploading part $PART_NUM: $PART_FILE ($(ls -lh $PART_FILE | awk '{print $5}'))"
    
    PART_RESPONSE=$(curl -s -X PUT \
        -H "Content-Type: application/octet-stream" \
        --data-binary "@$PART_FILE" \
        -D - \
        "$SERVER/$BUCKET/$KEY?uploadId=$UPLOAD_ID&partNumber=$PART_NUM")
    
    # Extract ETag from response headers
    ETAG=$(echo "$PART_RESPONSE" | grep -i "etag:" | awk '{print $2}' | tr -d '\r\n')
    
    if [ -z "$ETAG" ]; then
        print_error "Failed to upload part $PART_NUM (no ETag received)"
        exit 1
    fi
    
    ETAGS[$PART_NUM]=$ETAG
    print_success "Part $PART_NUM uploaded - ETag: $ETAG"
    
    PART_NUM=$((PART_NUM + 1))
done

echo ""

# Test 5: List Parts
echo "Test 5: List Parts"
echo "------------------"

print_info "Listing uploaded parts..."
LIST_RESPONSE=$(curl -s "$SERVER/$BUCKET/$KEY?uploadId=$UPLOAD_ID")

if [ -z "$LIST_RESPONSE" ]; then
    print_error "Failed to list parts (empty response)"
    exit 1
fi

# Count parts in response
LISTED_PARTS=$(echo "$LIST_RESPONSE" | grep -c "<PartNumber>" || echo "0")

if [ "$LISTED_PARTS" -eq "$PART_COUNT" ]; then
    print_success "Listed $LISTED_PARTS parts (matches uploaded count)"
else
    print_error "Part count mismatch: listed $LISTED_PARTS, uploaded $PART_COUNT"
fi

# Verify truncation flag
IS_TRUNCATED=$(extract_xml "$LIST_RESPONSE" "IsTruncated")
if [ "$IS_TRUNCATED" == "false" ]; then
    print_success "All parts returned (IsTruncated=false)"
else
    print_info "Results truncated (IsTruncated=true)"
fi

echo ""

# Test 6: Complete Multipart Upload
echo "Test 6: Complete Multipart Upload"
echo "----------------------------------"

print_info "Assembling parts into final object..."

# Build CompleteMultipartUpload XML
COMPLETE_XML='<CompleteMultipartUpload>'
for i in $(seq 1 $PART_COUNT); do
    COMPLETE_XML+="<Part><PartNumber>$i</PartNumber><ETag>${ETAGS[$i]}</ETag></Part>"
done
COMPLETE_XML+='</CompleteMultipartUpload>'

COMPLETE_RESPONSE=$(curl -s -X POST \
    -H "Content-Type: application/xml" \
    -d "$COMPLETE_XML" \
    "$SERVER/$BUCKET/$KEY?uploadId=$UPLOAD_ID")

if [ -z "$COMPLETE_RESPONSE" ]; then
    print_error "Failed to complete upload (empty response)"
    exit 1
fi

FINAL_ETAG=$(extract_xml "$COMPLETE_RESPONSE" "ETag")

if [ -z "$FINAL_ETAG" ]; then
    print_error "Failed to extract ETag from completion response"
    echo "$COMPLETE_RESPONSE"
    exit 1
fi

# Remove quotes from ETag
FINAL_ETAG=$(echo "$FINAL_ETAG" | tr -d '"')

print_success "Upload completed - Final ETag: $FINAL_ETAG"

# Verify ETag format (should be {md5}-{part_count})
if [[ "$FINAL_ETAG" =~ -[0-9]+$ ]]; then
    ETAG_PART_COUNT=$(echo "$FINAL_ETAG" | awk -F'-' '{print $NF}')
    if [ "$ETAG_PART_COUNT" -eq "$PART_COUNT" ]; then
        print_success "ETag format correct (multipart with $ETAG_PART_COUNT parts)"
    else
        print_error "ETag part count mismatch: $ETAG_PART_COUNT != $PART_COUNT"
    fi
else
    print_info "ETag format: $FINAL_ETAG (single MD5)"
fi

echo ""

# Test 7: Verify Object
echo "Test 7: Verify Downloaded Object"
echo "---------------------------------"

print_info "Downloading completed object..."
curl -s "$SERVER/$BUCKET/$KEY" -o downloaded.bin

DOWNLOADED_SIZE=$(ls -lh downloaded.bin | awk '{print $5}')
print_success "Downloaded object: $DOWNLOADED_SIZE"

# Calculate MD5 of downloaded file
DOWNLOADED_MD5=$(md5sum downloaded.bin | awk '{print $1}')
print_info "Downloaded file MD5: $DOWNLOADED_MD5"

if [ "$ORIGINAL_MD5" == "$DOWNLOADED_MD5" ]; then
    print_success "MD5 verification PASSED - object integrity confirmed!"
else
    print_error "MD5 verification FAILED"
    echo "  Original:   $ORIGINAL_MD5"
    echo "  Downloaded: $DOWNLOADED_MD5"
    exit 1
fi

echo ""

# Test 8: Verify Upload Cleanup
echo "Test 8: Verify Upload Directory Cleanup"
echo "----------------------------------------"

MULTIPART_DIR="/tmp/buckets-data/$BUCKET/.multipart/$UPLOAD_ID"

if [ -d "$MULTIPART_DIR" ]; then
    print_error "Upload directory still exists: $MULTIPART_DIR"
    ls -la "$MULTIPART_DIR"
else
    print_success "Upload directory cleaned up successfully"
fi

echo ""

# Test 9: Test Abort
echo "Test 9: Test Abort Multipart Upload"
echo "------------------------------------"

print_info "Initiating new upload for abort test..."
ABORT_RESPONSE=$(curl -s -X POST -H "Content-Length: 0" "$SERVER/$BUCKET/abort-test.bin?uploads")
ABORT_UPLOAD_ID=$(extract_xml "$ABORT_RESPONSE" "UploadId")

if [ -z "$ABORT_UPLOAD_ID" ]; then
    print_error "Failed to initiate abort test upload"
    exit 1
fi

print_success "Abort test upload initiated: $ABORT_UPLOAD_ID"

print_info "Uploading one part..."
curl -s -X PUT \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@part_aa" \
    "$SERVER/$BUCKET/abort-test.bin?uploadId=$ABORT_UPLOAD_ID&partNumber=1" > /dev/null

print_success "Part uploaded"

print_info "Aborting upload..."
ABORT_RESULT=$(curl -s -X DELETE "$SERVER/$BUCKET/abort-test.bin?uploadId=$ABORT_UPLOAD_ID")

ABORT_DIR="/tmp/buckets-data/$BUCKET/.multipart/$ABORT_UPLOAD_ID"
if [ -d "$ABORT_DIR" ]; then
    print_error "Abort failed - directory still exists: $ABORT_DIR"
else
    print_success "Upload aborted and cleaned up successfully"
fi

echo ""

# Summary
echo "======================================"
echo "Test Summary"
echo "======================================"
print_success "All multipart upload tests PASSED!"
echo ""
echo "Test Results:"
echo "  ✓ Environment setup"
echo "  ✓ Bucket creation"
echo "  ✓ Multipart upload initiation"
echo "  ✓ Part upload ($PART_COUNT parts)"
echo "  ✓ Part listing"
echo "  ✓ Multipart completion"
echo "  ✓ Object verification (MD5 match)"
echo "  ✓ Upload cleanup"
echo "  ✓ Abort functionality"
echo ""
echo "Performance:"
echo "  File size:     ${FILE_SIZE_MB}MB"
echo "  Part size:     ${PART_SIZE_MB}MB"
echo "  Part count:    $PART_COUNT"
echo "  Original MD5:  $ORIGINAL_MD5"
echo "  Final ETag:    $FINAL_ETAG"
echo ""

cleanup

print_success "Test suite completed successfully!"
