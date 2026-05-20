import json

# Import your submodules logically
from public import utils
from public import modals
from private import secret_processor

def handle_message(message_str):
    try:
        req = json.loads(message_str)
        action = req.get("action")
        
        # 1. Base Framework Example
        if action == "ping":
            data = req.get("data", "")
            return json.dumps({"status": "ok", "result": f"Pong! I received: {data}"})
            
        # 2. Public Module Example (e.g., formatting, generic API calls)
        elif action == "public_demo":
            user_name = req.get("name", "")
            greeting = utils.generate_greeting(user_name)
            return json.dumps({"status": "ok", "result": greeting})
            
        # 3. Private Module Example (e.g., database writes, OS file modifications)
        elif action == "private_demo":
            secret_data = req.get("secret_data", "")
            secure_hash_msg = secret_processor.process_secure_data(secret_data)
            return json.dumps({"status": "ok", "result": secure_hash_msg})
            
        # 4. Native System Dialog Examples (public/modals.py)
        elif action == "modal_alert":
            modals.show_alert(req.get("title", "Alert"), req.get("message", ""))
            return json.dumps({"status": "ok", "result": None})

        elif action == "modal_info":
            modals.show_info(req.get("title", "Info"), req.get("message", ""))
            return json.dumps({"status": "ok", "result": None})

        elif action == "modal_error":
            modals.show_error(req.get("title", "Error"), req.get("message", ""))
            return json.dumps({"status": "ok", "result": None})

        elif action == "modal_confirm":
            confirmed = modals.show_confirm(req.get("title", "Confirm"), req.get("message", ""))
            return json.dumps({"status": "ok", "result": confirmed})

        elif action == "modal_prompt":
            value = modals.show_prompt(req.get("title", "Input"), req.get("message", ""), req.get("default", ""))
            return json.dumps({"status": "ok", "result": value})

        return json.dumps({"status": "error", "reason": "Unknown action"})
    except Exception as e:
        return json.dumps({"status": "error", "reason": str(e)})
