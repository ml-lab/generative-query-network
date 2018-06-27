#include "renderer.h"
#include "opengl/functions.h"
#include <glm/gtc/type_ptr.hpp>

namespace three {
namespace renderer {
    void Renderer::initialize(int width, int height)
    {
        glfwSetErrorCallback([](int error, const char* description) {
            fprintf(stderr, "Error %d: %s\n", error, description);
        });
        if (!!glfwInit() == false) {
            throw std::runtime_error("Failed to initialize GLFW.");
        }

        _width = width;
        _height = height;
        _depth_buffer = std::make_unique<GLfloat[]>(width * height);
        _color_buffer = std::make_unique<GLubyte[]>(width * height * 3);
        _prev_num_objects = -1;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
#if __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
        _window = glfwCreateWindow(width, height, "Renderer", NULL, NULL);
        glfwMakeContextCurrent(_window);
        gl3wInit();

        _vao = std::make_unique<opengl::VertexArrayObject>();
        _depth_program = std::make_unique<multipass::Depth>();
        _main_program = std::make_unique<multipass::Main>();
        
        glGenRenderbuffers(1, &_render_buffer);
    }
    Renderer::Renderer(int width, int height)
    {
        initialize(width, height);
    }
    Renderer::Renderer(scene::Scene* scene, int width, int height)
    {
        initialize(width, height);
        set_scene(scene);
    }
    Renderer::~Renderer()
    {
        glfwDestroyWindow(_window);
        glfwTerminate();
    }
    void Renderer::set_scene(scene::Scene* scene)
    {
        glfwMakeContextCurrent(_window);
        _scene = scene;
        _vao->build(scene);
    }
    void Renderer::render_objects(camera::PerspectiveCamera* camera)
    {
        // OpenGL commands are executed in global context (per thread).
        glfwMakeContextCurrent(_window);
        glViewport(0, 0, _width, _height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0.0, 0.0, 0.0, 1.0);

        static float quadratic_attenuation = 2.0;

        glm::mat4& view_mat = camera->_view_matrix;
        glm::mat4& projection_mat = camera->_projection_matrix;
        std::vector<std::shared_ptr<base::Object>>& objects = _scene->_objects;
        for (int object_index = 0; object_index < objects.size(); object_index++) {
            std::shared_ptr<base::Object> object = objects[object_index];
            _vao->bind_object(object_index);
            glm::mat4& model_mat = object->_model_matrix;
            _main_program->uniform_matrix(0, glm::value_ptr(model_mat));
            _main_program->uniform_matrix(1, glm::value_ptr(view_mat));
            _main_program->uniform_matrix(2, glm::value_ptr(projection_mat));
            float smoothness = object->_smoothness ? 1.0 : 0.0;
            _main_program->uniform_float(3, smoothness);
            glDrawArrays(GL_TRIANGLES, 0, 3 * object->_num_faces);
        }
    }
    void Renderer::render_depth_map(
        camera::PerspectiveCamera* camera,
        py::array_t<GLfloat, py::array::c_style> np_depth_map)
    {
        if (glfwWindowShouldClose(_window)) {
            glfwDestroyWindow(_window);
            glfwTerminate();
            return;
        }

        _main_program->use();
        glEnable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindRenderbuffer(GL_RENDERBUFFER, _render_buffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _render_buffer);

        render_objects(camera);

        glReadPixels(0, 0, _width, _height, GL_DEPTH_COMPONENT, GL_FLOAT, _depth_buffer.get());
        auto depth_map = np_depth_map.mutable_unchecked<2>();
        for (int h = 0; h < _height; h++) {
            for (int w = 0; w < _width; w++) {
                depth_map(h, w) = _depth_buffer[(_height - h - 1) * _width + w];
            }
        }

        glUseProgram(0);
        glBindVertexArray(0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    void Renderer::render(
        camera::PerspectiveCamera* camera,
        py::array_t<GLuint, py::array::c_style> np_rgb_map)
    {
        if (glfwWindowShouldClose(_window)) {
            glfwDestroyWindow(_window);
            glfwTerminate();
            return;
        }

        _main_program->use();
        glEnable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glViewport(0, 0, _width, _height);

        glBindRenderbuffer(GL_RENDERBUFFER, _render_buffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _render_buffer);

        render_objects(camera);

        glReadPixels(0, 0, _width, _height, GL_RGB, GL_UNSIGNED_BYTE, _color_buffer.get());
        auto rgb_map = np_rgb_map.mutable_unchecked<3>();
        for (int h = 0; h < _height; h++) {
            for (int w = 0; w < _width; w++) {
                rgb_map(h, w, 0) = _color_buffer[(_height - h - 1) * _width * 3 + w * 3 + 0];
                rgb_map(h, w, 1) = _color_buffer[(_height - h - 1) * _width * 3 + w * 3 + 1];
                rgb_map(h, w, 2) = _color_buffer[(_height - h - 1) * _width * 3 + w * 3 + 2];
            }
        }

        glUseProgram(0);
        glBindVertexArray(0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    void Renderer::render_depth_map(scene::Scene* scene, camera::PerspectiveCamera* camera,
        py::array_t<GLfloat, py::array::c_style> np_depth_map)
    {
        set_scene(scene);
        render_depth_map(camera, np_depth_map);
    }
    void Renderer::render(scene::Scene* scene, camera::PerspectiveCamera* camera,
        py::array_t<GLuint, py::array::c_style> np_rgb_map)
    {
        set_scene(scene);
        render(camera, np_rgb_map);
    }
}
}