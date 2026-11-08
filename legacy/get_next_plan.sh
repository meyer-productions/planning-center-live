#!/bin/bash
#Scripted By Fabian Meyer for ICF Baselland
#In Assistance from ChatGPT
#Version 0.1

# Your credentials
APP_ID="your_app_id"
SECRET="your_secret"

#get your App ID Here: https://api.planningcenteronline.com/oauth/applications

# The name of the service type to search for
# When the script is started, all available celebrations are listed accordingly and can be entered here:
SERVICE_TYPE_NAME="RunScript once and fill in Here"

########################################################################################
########################################################################################
########################################################################################
# Function to list all service types
list_service_types() {
    response=$(curl -s -u "$APP_ID:$SECRET" -X GET "https://api.planningcenteronline.com/services/v2/service_types")
    
    if [[ $(echo "$response" | jq -r '.errors') != "null" ]]; then
        echo "Error getting service types: $(echo "$response" | jq -r '.errors[0].detail')"
        exit 1
    fi

    # List the names and IDs of all service types
    echo "Available Service Types:"
    echo "$response" | jq -r '.data[] | "ID: \(.id), Name: \(.attributes.name)"'
}

# Function to get the service type ID
get_service_type_id() {
    response=$(curl -s -u "$APP_ID:$SECRET" -X GET "https://api.planningcenteronline.com/services/v2/service_types")
    
    if [[ $(echo "$response" | jq -r '.errors') != "null" ]]; then
        echo "Error getting service types: $(echo "$response" | jq -r '.errors[0].detail')"
        exit 1
    fi

    # Extract the ID for the specified service type name
    echo "$response" | jq -r --arg name "$SERVICE_TYPE_NAME" '.data[] | select(.attributes.name == $name) | .id'
}

# Function to get all plans for the specified service type ID with offset handling
get_all_plans() {
    service_type_id=$1
    all_plans="[]"
    offset=256

    while [ $offset -le 400 ]; do
        response=$(curl -s -u "$APP_ID:$SECRET" -X GET "https://api.planningcenteronline.com/services/v2/service_types/$service_type_id/plans?offset=$offset")
        
        if [[ $(echo "$response" | jq -r '.errors') != "null" ]]; then
            echo "Error getting service plans: $(echo "$response" | jq -r '.errors[0].detail')"
            exit 1
        fi

        plans=$(echo "$response" | jq '.data')
        if [[ "$plans" == "[]" ]]; then
            break
        fi

        all_plans=$(echo "$all_plans $plans" | jq -s 'add')
        offset=$((offset + 20))
    done

    echo "$all_plans"
}

# Function to get the next or current plan ID and date
get_next_plan() {
    plans=$1
    today=$(date +%Y-%m-%d)

    next_plan=$(echo "$plans" | jq -r --arg today "$today" '
        map(select(.attributes.dates | gsub(" "; "-") | strptime("%d-%B-%Y") | mktime >= ($today | strptime("%Y-%m-%d") | mktime)))
        | sort_by(.attributes.dates | gsub(" "; "-") | strptime("%d-%B-%Y") | mktime)
        | .[0] | {id: .id, dates: .attributes.dates}
    ')
    echo "$next_plan"
}

# Main script

# List all available service types
list_service_types

service_type_id=$(get_service_type_id)
echo "Selected service Type ID: $service_type_id"

if [ -z "$service_type_id" ]; then
    echo "Service Type ID for '$SERVICE_TYPE_NAME' not found."
    exit 1
fi

plans=$(get_all_plans "$service_type_id")
echo "All Plans for $SERVICE_TYPE_NAME:"
echo "$plans" | jq -r '.[] | "ID: \(.id), dates: \(.attributes.dates)"'

next_plan=$(get_next_plan "$plans")
next_plan_id=$(echo "$next_plan" | jq -r '.id')
next_plan_date=$(echo "$next_plan" | jq -r '.dates')
echo -e "\n########################################################################\nNext Plan ID: $next_plan_id ($next_plan_date) for $SERVICE_TYPE_NAME \n########################################################################"

# Open browser with the URL in full screen mode
if [ -n "$next_plan_id" ]; then
    url="https://services.planningcenteronline.com/live/$next_plan_id"
    echo "Opening browser with URL: $url"
    nohup firefox --new-window --kiosk "$url" >/dev/null 2>&1 &
else
    echo "No next plan found."
fi
