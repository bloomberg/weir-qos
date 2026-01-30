QOS = {
    UNCLASSIFIED_OP = "",
    CONFIGURED_OPS = {
        ["LISTOBJECTSV2"] = false,
        ["LISTMULTIPARTUPLOADS"] = false,
        ["LISTOBJECTVERSIONS"] = false,
        ["LISTBUCKETS"] = false,
        ["LISTOBJECTS"] = false,
        ["DELETEOBJECTS"] = false,
        ["DELETEOBJECT"] = false,
        ["CREATEBUCKET"] = false
    }
}

function get_bucket_name(path, host)
    -- there are two formats of speicifying bucket: bucket.s3.zone.dc.com/object or s3.zone.dc.com/bucket/object
    -- we are assuming that the url would have s3......com format here and the assumption is safe in our prod env. Our VM env doesn't have
    -- this format.
    first_dot = host:find(".", 1, true)
    if first_dot and first_dot > 1  and (host:sub(first_dot + 1, first_dot + 3) == "s3." or host:sub(first_dot + 1, first_dot + 3) == "S3.") then --bucket is in host
        return host:sub(1, first_dot - 1)
    else  -- we shold find bucket from path
        local bucket_start_idx = 1
        if path:sub(1, 1) == "/" then
            bucket_start_idx = 2
        end
        local bucket_end_idx = path:find("/", bucket_start_idx)
        if bucket_end_idx == nil then
            bucket_end_idx = #path
        else
            bucket_end_idx = bucket_end_idx - 1
        end
        return path:sub(bucket_start_idx, bucket_end_idx)

    end
end

function get_bucket(headers, txn)
    local bucket = ""
    if headers and headers["host"] and headers["host"][0] then
        bucket = get_bucket_name(txn.f:path(), headers["host"][0])
    end
    return bucket
end

local function validate_access_key(access_key)
    -- an accesskey must be an alphanumeric string with a length of 20 chars
    -- "common" is a special accesskey used when no valid accesskey is located
    if access_key == nil or string.len(access_key) == 0 then
        return "common"
    elseif string.len(access_key) == 20 and not string.match(access_key, "%W") then
        return access_key
    -- REMOVE: This is put in place to temporarily support STS.
    elseif string.len(access_key) == 19 and not string.match(access_key, "%W") then
        return access_key
    else
        core.Warning("Invalid access key: " .. access_key)
        return "ItIsInvalidAccessKey" -- a special key with length of 20 chars
    end
end

local function parse_access_key_from_query_params(query_params)
    local access_key = ""

    if query_params then
        if query_params["x-amz-credential"] then
            -- For AWS Auth v4: https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-query-string-auth.html
            -- X-Amz-Credential=<your-access-key-id>/<date>/<AWS-Region>/<AWS-service>/aws4_request
            access_key = string.match(query_params["x-amz-credential"], "^(%w+)")
        elseif query_params["awsaccesskeyid"] then
            -- For AWS Auth v2: https://docs.aws.amazon.com/general/latest/gr/signature-version-2.html
            -- AWSAccessKeyId=<your-access-key-id>
            access_key = string.match(query_params["awsaccesskeyid"], "^(%w+)")
        end
    end

    access_key = validate_access_key(access_key)
    return access_key
end

local function parse_access_key_from_auth_header(auth_str)
    -- If headers are for a HTTP response, they should be empty
    -- and there will be no Authorization header

    -- Check whether Authorization header is present, and save it
    local auth_method = auth_str:sub(1, 4)
    local access_key_start_idx = nil
    -- Access key always is of length 20 when system generated
    if auth_method == "AWS4" then
        -- For AWS Auth v4
        -- AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,

        -- Access key starts at index 29
        access_key_start_idx = 29

   elseif auth_method == "AWS " then
        -- For AWS Auth v2
        -- AWS AKIAIOSFODNN7EXAMPLE:frJIUN8DYpKDtOLCwo//yllqDzg=
        -- Access key starts at index 5
        access_key_start_idx = 5
    else
        core.Warning("Invalid Authorization header format" .. auth_method)
        return "InvalidAuthorization" -- a special key with a length of 20
    end
    -- we can assume all keys are 20 chars as this is an S3 requirement.
    local access_key = auth_str:sub(access_key_start_idx, access_key_start_idx + 19)
    -- REMOVE: For STS support, access keys are 19 chars due to a Ceph bug so we need to trim the trailing "/".
    -- The following lines should be removed once Ceph bug is fixed.
    if string_endswith(access_key, "/") then
        access_key = access_key:sub(0, #access_key-1)
    end

    return validate_access_key(access_key)
end

local function classify_request_class(txn)
    local path = txn.f:path() or ""
    local query = txn.f:query() or ""
    local method = txn.f:method() or ""
    local uri = path
    if query ~= "" then
        uri = uri .. "?" .. query
    end

    local op = QOS.UNCLASSIFIED_OP

    -- Known control-plane query parameters
    local control_plane_params = {
        acl=true, policy=true, tagging=true, versioning=true, location=true,
        publicAccessBlock=true, cors=true, encryption=true, lifecycle=true,
        website=true, notification=true, logging=true, replication=true,
        metrics=true, inventory=true, ["object-lock"]=true, analytics=true,
        ownershipControls=true, requestPayment=true, accelerate=true,
        ["intelligent-tiering"]=true, torrent=true, metadata=true,
        metadataTable=true
    }

    -- Parse query keys into a set
    local query_keys = {}
    for k in query:gmatch("([^&=?]+)") do
        query_keys[k] = true
    end

    -- Detect if any known control-plane key is present
    local is_control_plane = false
    for k in pairs(query_keys) do
        if control_plane_params[k] then
            is_control_plane = true
            break
        end
    end

    local trimmed = path:gsub("^/", "")

    if method == "GET" then
        if query_keys["list-type"] then
            op = "LISTOBJECTSV2"
        elseif query_keys["uploads"] then
            op = "LISTMULTIPARTUPLOADS"
        elseif query_keys["versions"] then
            op = "LISTOBJECTVERSIONS"
        elseif not is_control_plane and uri == "/" then
            op = "LISTBUCKETS"
        elseif not is_control_plane and query ~= "" and not trimmed:find("/") then
            op = "LISTOBJECTS"
        end

    elseif method == "POST" then
        if query_keys["delete"] then
            op = "DELETEOBJECTS"
        end

    elseif method == "DELETE" then
        if not query_keys["uploadId"] and not is_control_plane and trimmed:find("/") then
            op = "DELETEOBJECT"
        end

    elseif method == "PUT" then
        if not is_control_plane and not trimmed:find("/") and query == "" then
            op = "CREATEBUCKET"
        end
    end

    -- Final allowlist control
    local raw_op = op
    if not QOS.CONFIGURED_OPS[op] then
        op = QOS.UNCLASSIFIED_OP
    end

    core.Debug(string.format(
        "[classify_request_class] method=%s uri=%s classified=%s",
        method, uri, raw_op, op
    ))

    return op
end

-- This is specifically not a local function so that other lua scripts can access it.
-- This is useful if you have any other features you'd like to allow only for some users.
function get_access_key(headers, query_params)
    local access_key = ""
    if headers and headers["authorization"]  and headers["authorization"][0] then
        access_key = parse_access_key_from_auth_header(headers["authorization"][0])
    else
        access_key = parse_access_key_from_query_params(query_params)
    end

    return access_key
end

-- this method is used to get the user access key for haproxy logging
core.register_fetches("get_user_access_key", function(txn)
    local headers = txn.http:req_get_headers()
    local query_params = get_decoded_query_params(txn.f:query())
    local access_key = get_access_key(headers, query_params)
    return access_key
end)

core.register_fetches("get_s3_op_direction", function(txn)
    local method = txn.f:method()
    if method == "PUT" or method == "POST" then
        return "up"
    else
        return "dwn"
    end
end)

core.register_fetches("weir_should_block_s3_request", function(txn)
    local headers = txn.http:req_get_headers()
    local query_params = get_decoded_query_params(txn.f:query())
    local access_key = get_access_key(headers, query_params)
    local op_class = classify_request_class(txn)
    txn:set_var("txn.op_class", op_class)
    return weir_should_block_request(txn, access_key, op_class)
end)
