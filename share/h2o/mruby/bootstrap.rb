module H2O

  class ConfigurationContext
    def self.instance()
      @@instance
    end
    def self.instance=(instance)
      @@instance = instance
    end
    def initialize()
      @values = {}
      @post_handler_generation_hooks = []
    end
    def get_value(key)
      @values[key]
    end
    def set_value(key, value)
      @values[key] = value
    end
    def delete_value(key)
      @values[key].delete
    end
    def add_post_handler_generation_hook(hook)
      @post_handler_generation_hooks << hook
    end
    def post_handler_generation(handler)
      @post_handler_generation_hooks.each {|hook| hook.call(handler) }
    end
  end

end
