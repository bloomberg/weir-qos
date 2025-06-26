-- Copyright 2024 Bloomberg Finance L.P.
if not _G.__haproxy_lua_loaded then
  _G.__haproxy_lua_loaded = true

require("os")
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

-- Returns true if `needle` forms a suffix for `haystack`.
-- Both arguments are plain strings, not patterns.
-- The empty string is considered a suffix for any `haystack`.
-- The empty string itself has no non-empty suffixes.
function string_endswith(haystack, needle)
    if #needle > #haystack then
        return false
    end
    local substack = string.sub(haystack, #haystack-#needle + 1, #haystack + 1)
    return substack == needle
end

-- Returns a list of substrings of `input` containing the substrings
-- before *and* after every instance of `delimiter`.
-- Repeated instances of the delimiter will not be coalesced,
-- resulting in empty strings in the output list.
-- If the input begins or ends with the delimiter, the output will begin
-- or end with an empty string respectively.
function string_split(input, delimiter, max_entries)
    if #delimiter == 0 then
        return {input}
    end
    local result = {}
    local from = 1
    while from <= #input do
        local delim_start, delim_end = string.find(input, delimiter, from, true)
        -- If we don't find the delimiter or have reached the max entries
        -- already then the last item is just the rest of the string.
        if (delim_start == nil) or (max_entries ~= nil and #result+1 >= max_entries) then
            table.insert(result, string.sub(input, from))
            break
        end

        table.insert(result, string.sub(input, from, delim_start-1))
        from = delim_end + 1
    end

    if string_endswith(input, delimiter) then
        table.insert(result, "")
    end

    return result
end

function validate_access_key(access_key)
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


function parse_access_key_from_query_params(query_params)
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


function parse_access_key_from_auth_header(auth_str)
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

vio_map = {}
reqs_map = {}

function is_reqs_violator(curr_time, user)
    --[[
    Check if the requests map contains the user, which indicates whether to block the
    request. The value is the epoch when we last received a violation message. Normally,
    we don't care about the value but for cases where we can potentially lose connection
    with polygen and stop receiving policies, we are in an unsure state about whether
    this user should remain blocked. In that case we wait ~2 seconds in order to allow
    requests to potentially drain and unblock the user to avoid keeping them blocked
    indefinitely.
    ]]--
    if reqs_map[user] == nil then
        return false
    else
        return (reqs_map[user] + 2) > curr_time
    end
end

-- check if current request is a violater, we need to check all catogories.
function is_violater(cur_time, user, method)
    if is_reqs_violator(cur_time, user) then
       return 1, "requests"
    end     
    -- Example: user="F1T" method="GET"
    local vio_key = "user_" .. method
    if vio_map[vio_key] and vio_map[vio_key][cur_time] and vio_map[vio_key][cur_time][user] then
        core.Debug(vio_key .." exceeded," .."user="..user)
        return 1, "rate"
    end
    return 0, ""
end

function get_access_key(headers, query_params)
    local access_key = ""
    if headers and headers["authorization"]  and headers["authorization"][0] then
        access_key = parse_access_key_from_auth_header(headers["authorization"][0])
    else
        access_key = parse_access_key_from_query_params(query_params)
    end

    return access_key
end

function parse_url_query_string_into_params_table(query_string)
    local result = {}

    if query_string == nil or query_string == "" then
        return result
    end

    for k,v in query_string:gmatch('([^&=?]-)=([^&=?]+)' ) do
        result[string.lower(k)] = v
    end

    return result
end


function get_decoded_query_params(query_string)
    local query_params = {}
    local hex_to_char = function(i)
        return string.char(tonumber(i, 16))
    end

    if query_string == nil or query_string == "" then
        return query_params
    end

    query_string = query_string:gsub("%%(%x%x)", hex_to_char) -- decode URL encoding
    query_params = parse_url_query_string_into_params_table(query_string)

    return query_params
end

local function check_violation(epoch, user, method_or_op, txn, tag)
    local violater, vio_type = is_violater(epoch, user, method_or_op)
    if violater ~= 0 then
        local verb = method_or_op
        local size = txn.sf:req_fhdr("Content-Length") or "N/A"
        local label = tag and (vio_type .. tag) or vio_type
        core.Info(string.format(
            "%s limiting denied request src=%s_%s user=%s verb=%s size=%s",
            label, txn.f:src(), txn.f:src_port(), user, verb, size
        ))
    end
    return violater
end

-- this is the entry point of http request
-- first send the request to aggregator
-- then check of if we should throttle
core.register_fetches("req_eval", function(txn)
    local headers = txn.http:req_get_headers()
    local query_params = get_decoded_query_params(txn.f:query())
    local access_key = get_access_key(headers, query_params)
    txn.http:req_set_ip_port_key(txn.f:src(), txn.f:src_port(), access_key)
    local op_class = classify_request_class(txn)
    txn:set_var("txn.op_class", op_class)

    local epoch = os.time()
    local violater = check_violation(epoch, access_key, txn.f:method(), txn)
    if violater ~= 0 then
        return violater
    end
    -- Check for classified ops violation
    if op_class ~= QOS.UNCLASSIFIED_OP then
        violater = check_violation(epoch, access_key, op_class, txn, "ops")
        if violater ~= 0 then
            return violater
        end
    end
end)

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

function classify_request_class(txn)
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





-- process violates
function update_violates(line, curr_time)
    -- Note: epochs are in usec resolution
    -- Example: 1554318336056480,user_GET,AKIAIOSFODNN7EXAMPLE,AKIAIOSFODNN8EXAMPLE
    -- Example: 1682013607056577,user_bnd_up,AKIAIOSFODNN7EXAMPLE:2.7,AKIAIOSFODNN8EXAMPLE:2.4
    -- Example: user_reqs_block,AKIAIOSFODNN7EXAMPLE
    -- Note that only "bnd" metrics has violation diff ratio
    local items = string_split(line, ",")
    if #items < 2 then
        core.Warning("Received invalid violation: " .. line)
        return
    end
    
    if string.match(items[1], "_reqs_") then
        update_violates_reqs(items, curr_time)
        return
    end

    if #items < 3 then
        core.Warning("Received invalid violation: " .. line)
        return
    end

    local poli_time_us = tonumber(items[1])
    local poli_time = poli_time_us // 1000000
    if poli_time < curr_time  then --it is too late
        core.Warning("Received stale policy: " .. line .. " | Current time: " .. curr_time)
        return
    end

    if string.match(items[2], "_bnd_") then
        update_violates_epoch(items, poli_time_us)
    else
        update_violates_map(items, poli_time)
    end
end

function update_violates_reqs(items, curr_time)
    local should_close
    if string.match(items[1], "reqs_block") then
        should_close = 1
    elseif string.match(items[1], "reqs_unblock") then
        should_close = 0
    else
        core.Warning("Received invalid violation: " .. table.concat(items, ","))
        return
    end

    for i, v in ipairs(items) do
      if i > 1 then
        if should_close == 1 then
            reqs_map[v] = curr_time
        else
            reqs_map[v] = nil
        end
      end
    end
end


-- update violate epoch
function update_violates_epoch(items, poli_time_us)
    local idx = items[2]:find("_")
    if idx == nil then
        return
    end

    -- get key type (ip/user/buc)
    local key_type = items[2]:sub(1, idx-1)

    -- items[2]
    local up_dwn
    if string.match(items[2], "_up") then
        up_dwn = "upload"
    elseif string.match(items[2], "_dwn") then
        up_dwn = "download"
    else
        core.Warning("Received invalid violation: " .. table.concat(items, ","))
        return
    end

    for k, v in ipairs(items) do
        if k > 2 then
            local acc_key = "unknown"
            local diff_ratio = "1.0"
            local key_ratio_pair = string_split(v, ":")
            if #key_ratio_pair == 2 then
                acc_key = key_ratio_pair[1]
                diff_ratio = key_ratio_pair[2]
            elseif #key_ratio_pair == 1 then
                acc_key = key_ratio_pair[1]
            end
            local key = key_type.."_"..acc_key
            core.Debug("Throttle key "..key.." "..up_dwn.." "..poli_time_us)
            local ret = core.throttle_key_speed(acc_key, up_dwn, poli_time_us, diff_ratio)
            if not ret then
                core.Err("Failed to set throttle: "..key.." "..up_dwn.." "..poli_time_us)
            end
        end
    end
end

-- update violate map
function update_violates_map(items, curr_time)
    if not vio_map[items[2]] then
        vio_map[items[2]] = {}
    end
    map_update = vio_map[items[2]]
    -- remove old data
    for k, v in pairs(map_update) do
        if k + 3 < curr_time then
            map_update[k] = nil
        end
    end
    if not map_update[curr_time] then
        map_update[curr_time] = {}
    end

    for k, v in ipairs(items) do
      if k > 2 then
        map_update[curr_time][v] = 1
      end
    end    
end

function ingest_policies(applet)
    -- Tesing listening port hooks manually:
    -- 1. add the command string to a file, for example:
    --    $ echo "set_jitter_range:20" > cmd.txt
    -- 2. netcat the file to the listening port by following the below example
    --    $ netcat 1.2.3.4 10001 < cmd.txt
    while true do
        local inputs = applet:getline()
        if inputs == nil or string.len(inputs) == 0 then
            core.Info("closing a policy generator connection")
            return
        end        
        -- Example: policies\n1554317654000000,user_GET,AKIAIOSFODNN7EXAMPLE,AKIAIOSFODNN8EXAMPLE\n1554317654555000,ip_PUT,1.2.3.4
        -- Example: policies\n1682013607888000,user_bnd_up,AKIAIOSFODNN7EXAMPLE:2.7,AKIAIOSFODNN8EXAMPLE:2.4
        -- Note that only "bnd" metrics has violation diff ratio
        if string.find(inputs, "policies", 1, true) == 1 then
            -- this is a QoS policy issued by policy-generator
            curr_time = os.time()
            while true do
                inputs = applet:getline()
                inputs = inputs:gsub("^%s*(.-)%s*$", "%1")                
                local first_pos = string.find(inputs, "END_OF_POLICIES", 1, true)
                if first_pos == 1 then
                    break
                end
                core.Debug(inputs)
                update_violates(inputs, curr_time)
            end

        elseif string.find(inputs, "limit_share", 1, true) == 1 then
            -- Example:
            -- limit_share
            -- 1234567,myaccesskey,1f2e3d4c_up_1024,1f2e3d4c_down_4096,999dead0_down_8192
            -- end_limit_share
            local has_error = false
            while has_error == false do
                inputs = applet:getline()
                if string.find(inputs, "end_limit_share", 1, true) == 1 then
                    break
                end

                if string.find(inputs, "limit_share", 1, true) == 1 then
                    core.Warning("New limit-share message started before the previous one finished, some data could have been dropped")
                else
                    local components = string_split(inputs, ",")
                    if #components < 3 then
                        core.Warning("Received invalid limit-share update with too few components: "..inputs)
                        break
                    end

                    local timestamp = tonumber(components[1], 10)
                    local user_key = components[2]
                    if timestamp == nil then
                        core.Warning("Received invalid limit-share update with invalid timestamp: "..inputs)
                        break
                    end

                    for i=3, #components do
                        local instance_components = string_split(components[i], "_")
                        if #instance_components ~= 3 then
                            core.Warning("Received invalid instance limit-share update with too few components: "..components[i].." in component "..i.." of input line: "..inputs)
                            has_error = true
                            break
                        end

                        local instance_id = instance_components[1]
                        local direction = instance_components[2]
                        local limit = tonumber(instance_components[3], 10)
                        if limit == nil then
                            core.Warning("Received invalid instance limit-share update with invalid limit: "..components[i].." in component "..i.." of input line: "..inputs)
                            has_error = true
                            break
                        end

                        core.ingest_weir_limit_share_update(timestamp, user_key, instance_id, direction, limit)
                    end
                end
            end

        else
            core.Warning("Unmatched policy message:: "..inputs)
        end
    end
end
-- entry point to get policies.
-- we also provide a admin port here
core.register_service("ingest_policies", "tcp", ingest_policies)

end
