-- Copyright 2024 Bloomberg Finance L.P.
if not _G.__haproxy_lua_loaded then
  _G.__haproxy_lua_loaded = true

require("os")

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

function weir_should_block_request(txn, request_key, op_class)
    --[[
    Check if Weir thinks an incoming request should be blocked.
    This should be called early on in request processing. In particular
    it should be called *before* the activate-weir action in config.

    Returns 1 if the request should be blocked, otherwise 0.

    Arguments:
    * txn: The transaction to consider for blocking.
    * request_key: The key that identifies this request's QoS policy.
                   In S3 for example, this could be the user's access key or the bucket name.
    * op_class: The type of protocol-specific operation represented by this request.
                This is used in combination with operation-specific limits to allow
                enforcing lower limits on certain operations (e.g if they're expensive).
                In S3 for example, this might be bulk deletes or bucket listings.
                If empty string ("") is passed, operation-specific checks are skipped.
                Pass an empty string ("") if you do not need operation-specific limits.
    ]]
    txn.http:req_set_ip_port_key(txn.f:src(), txn.f:src_port(), request_key)

    local epoch = os.time()
    local violater = check_violation(epoch, request_key, txn.f:method(), txn)
    if violater ~= 0 then
        return violater
    end
    -- Check for classified ops violation
    if op_class ~= "" then
        violater = check_violation(epoch, request_key, op_class, txn, "ops")
        if violater ~= 0 then
            return violater
        end
    end
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

-- STS QoS changes start here
function if_sts_credential_req(headers)
    local sec_header=headers['x-amz-security-token']
    if  sec_header and sec_header ~= nil then
        local sts_header=headers['x-amz-security-token'][0]
        if sts_header  and sts_header ~= nil then
          return 1
        else
          return 0
        end
    else
        return 0
    end
end


function sts_qos_populate_txn_context(txn)
    -- here we are evaluating to see of this is an sts assume role request and if yes, we attach an 
    -- "if_body_parse" property in the http req txn context. This will be available in the http resp txn context and we can 
    -- parse the bodies of those transactions for getting useful info. e.g. StsToken-Role mapping

    local content_length=tonumber(txn.sf:req_fhdr("Content-Length"))  
    -- Transactions which need body parsing - This is performance degrading , so enable it only for a small subset of transactions
    -- The transactions which have this enabled should be rated limited ideally   
    local body_parse_txns = {["AssumeRole"] = true}

    -- this filters out many of the data plane operations
    if content_length and type(content_length) == "number" and content_length > 0 then
        -- Note that this nested to filter out some of the operations that happens in haproxy layer
        local url_path=txn.f:path()
        if url_path ~= nil and url_path == "/" then  
            local req_headers_temp = txn.http:req_get_headers()
            local req_body_param=(txn.f:req_body_param() == nil) and "" or txn.f:req_body_param() 
            -- set flag in the txn context for the request if there is a content length and url path is and 
            -- if the body param is in the list of body parse txns. 
            if body_parse_txns[req_body_param] then 
                -- set_var sets it in the txn scope
                txn:set_var("txn.if_body_parse", "yes") 
            end
        end
    end
    -- for printing the STS tokens when a client uses them
    local req_headers = txn.http:req_get_headers()
    if if_sts_credential_req(req_headers) == 1 then
        -- Commenting out temporarily to prevent possible cred leaks 
        --core.Info("accesskeyid-ststoken~|~"..req_headers['authorization'][0].."~|~"..req_headers['x-amz-security-token'][0])
        core.Info("accesskeyid-ststoken~|~accesskeyststoken to follow")
    end
end

core.register_action("sts_qos_populate_txn_context", { "http-req" }, sts_qos_populate_txn_context)

-- STS QoS changes end here